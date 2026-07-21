// SPDX-License-Identifier: GPL-3.0-or-later
// om_hook - tracks CSP's per-eye scene render targets and injects DLSS + jitter.
// OMSetRenderTargets is vtable idx 33, PSSetShaderResources idx 8, VSSetConstantBuffers
// idx 7 on ID3D11DeviceContext; shared vtable, patched once.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include "ctx_backup.h"
#include "jitter_add_cs.h"   // compiled jitter_add.hlsl

extern "C" void acre_log(const char *fmt, ...);
extern "C" void acre_dlss_inplace(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                  ID3D11Texture2D *color, ID3D11Texture2D *depth,
                                  int eye, uintptr_t cam);
extern "C" void acre_dlss_inplace_sps(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                      ID3D11Texture2D *wide_color, ID3D11Texture2D *wide_depth,
                                      uintptr_t cam);
extern "C" void acre_sps_capture_depth_submit(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                              ID3D11Texture2D *wide_color, ID3D11Texture2D *wide_depth,
                                              uintptr_t cam);
extern "C" void acre_up_capture_depth(ID3D11DeviceContext *ctx, ID3D11Texture2D *depth, int eye);
extern "C" int acre_cfg_mode(void);
extern "C" int acre_cfg_jitter(void);
extern "C" float acre_cfg_jitter_scale(void);
extern "C" int acre_cfg_ldr(void);
extern "C" long acre_present_count(void);   // frame clock, in dxgi_hook.cpp
extern "C" void acre_diag_hooks(int ok);
extern "C" void acre_diag_scene(unsigned w, unsigned h, unsigned samples, unsigned arraysize);
extern "C" void acre_diag_eye(int eye);
extern "C" void acre_diag_matrices(const float *cb, uintptr_t cam);
extern "C" void acre_diag_jitter_subs(int e0, int e1);
extern "C" int acre_cap_active(void);
extern "C" void acre_cap_in(ID3D11Device *, ID3D11DeviceContext *, int, ID3D11Texture2D *);
extern "C" void acre_cap_endframe(void);
extern "C" void acre_cap_eyes(ID3D11Device *, ID3D11DeviceContext *,
                              ID3D11Texture2D *, ID3D11Texture2D *);

static uintptr_t deref(uintptr_t p, uintptr_t off);   // defined at file scope below

namespace {

typedef void (STDMETHODCALLTYPE *OMSet_t)(ID3D11DeviceContext *, UINT,
                                          ID3D11RenderTargetView *const *,
                                          ID3D11DepthStencilView *);
OMSet_t g_orig_om = nullptr;
volatile LONG g_logged = 0;

const char *fmtname(unsigned f) {
    switch (f) {
    case DXGI_FORMAT_R32_FLOAT: return "R32F";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TL";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TL";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case DXGI_FORMAT_D32_FLOAT: return "D32F";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32X8X24";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
    default: return "?";
    }
}

// A compact signature so each distinct binding logs once.
// ra/da (ArraySize) and rs/ds (sample count) are part of the signature, not just the
// log text: under Single Pass Stereo a 2-slice array target has the SAME width/height
// as the per-eye target we already match on, so without ArraySize here the two are
// indistinguishable and the SPS case would silently look normal.
struct Sig { unsigned rw, rh, rf, ra, rs, dw, dh, df, da, ds; };
Sig g_seen[64];
int g_seen_n = 0;

bool seen(const Sig &s) {
    for (int i = 0; i < g_seen_n; i++)
        if (!memcmp(&g_seen[i], &s, sizeof(Sig))) return true;
    if (g_seen_n < 64) g_seen[g_seen_n++] = s;
    return false;
}

// a/s default to 0 so callers that only want w/h/f can pass nothing.
void describe_rtv(ID3D11RenderTargetView *rtv, unsigned &w, unsigned &h, unsigned &f,
                  unsigned *a = nullptr, unsigned *s = nullptr) {
    w = h = f = 0;
    if (a) *a = 0;
    if (s) *s = 0;
    if (!rtv) return;
    ID3D11Resource *res = nullptr;
    rtv->GetResource(&res);
    if (!res) return;
    ID3D11Texture2D *t = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t)) && t) {
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); w = d.Width; h = d.Height; f = d.Format;
        if (a) *a = d.ArraySize;
        if (s) *s = d.SampleDesc.Count;
        t->Release();
    }
    res->Release();
}
void describe_dsv(ID3D11DepthStencilView *dsv, unsigned &w, unsigned &h, unsigned &f,
                  unsigned *a = nullptr, unsigned *s = nullptr) {
    w = h = f = 0;
    if (a) *a = 0;
    if (s) *s = 0;
    if (!dsv) return;
    ID3D11Resource *res = nullptr;
    dsv->GetResource(&res);
    if (!res) return;
    ID3D11Texture2D *t = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t)) && t) {
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); w = d.Width; h = d.Height; f = d.Format;
        if (a) *a = d.ArraySize;
        if (s) *s = d.SampleDesc.Count;
        t->Release();
    }
    res->Release();
}

void *res_of_rtv(ID3D11RenderTargetView *rtv) {
    if (!rtv) return nullptr;
    ID3D11Resource *r = nullptr; rtv->GetResource(&r);
    if (r) r->Release();  // borrow the pointer identity only
    return r;
}
void *res_of_dsv(ID3D11DepthStencilView *dsv) {
    if (!dsv) return nullptr;
    ID3D11Resource *r = nullptr; dsv->GetResource(&r);
    if (r) r->Release();
    return r;
}

volatile LONG g_seq = 0;
volatile LONG g_pereye_logged = 0;
void *g_eye_tex[2] = {nullptr, nullptr};   // viveData[0/1].renderEye textures
volatile LONG g_eye_logged = 0;

// CSP renders each eye's scene into a shared RGBA16F colour + R32 depth, then reads that
// colour as a PS input to post-process it into renderEye.
bool g_dlss_enabled = true;
int g_upscale_mode = -1;
ID3D11Device *g_om_dev = nullptr;
ID3D11Texture2D *g_scene_color = nullptr;   // CSP's per-eye scene HDR (borrowed ptr)
ID3D11Texture2D *g_scene_depth = nullptr;
bool g_scene_dirty = false;                 // scene freshly rendered, not yet DLSS'd
// Single Pass Stereo: the scene target is one double-wide texture (width = 2*renderWidth)
// holding both eyes side-by-side, produced in a single instanced pass. When true, the
// injection point processes BOTH eyes at once (see hkPSSRV) instead of sequencing them
// across two passes. Detected purely by shape, so it also lights up if a user's CSP is
// set to a per-eye layout that still presents a double-wide HDR target.
bool g_scene_wide = false;
// cam+2512 is per-eye but rewritten before the dispatch point reads it, so both eyes were
// reprojecting from the same camera (diag showed separation 0.0000 on every frame).
// Bind-time was too EARLY (kglSetRenderTargets precedes viveRenderPass's view write:
// separation showed one-frame-stale values under camera motion). Snapshot on the first
// VSSetConstantBuffers of the scene pass instead — the CB cross-check proved cam+2512
// matches CSP's render view exactly there.
float g_eye_view[2][16];
bool g_snap_done[2] = {false, false};
volatile LONG g_eye_view_valid = 0;         // bit per eye

// AC renders spectate/external cameras MONO in VR (flat projection): there is no second
// eye render, and the "second" scene-texture PS bind is the same image again. Without
// this guard the second dispatch re-ran DLAA on eye 0's output (double-smoothing +
// ghost trails, confirmed by capture: eye1-in ≡ eye0-out). Stereo only when the camera
// is the drivable car's cockpit.
uintptr_t g_mod_base = 0;
bool g_stereo = true;
// CSP rebinds the scene target mid-post-processing; dispatching on those rebinds ran
// DLAA on eye 0's own output and consumed the eye counter, leaving the REAL eye 1
// render unprocessed (pit-stereo capture: eye1-in ≡ eye0-out while raw inputs match).
// Draw-count and depth-clear discriminators were tried and both failed (CSP doesn't
// clear the scene depth via ClearDepthStencilView, and its geometry submission doesn't
// go through any hookable ID3D11DeviceContext draw entry — GPU-driven / NVAPI path).
// Authoritative discriminator instead: CSP ends each eye's post-processing by binding
// viveData[eye].renderEye as the render target (pointers in g_eye_tex). Eye sequencing
// keys off those binds; at most one dispatch per eye per frame, and mid-PP scene
// rebinds before the renderEye landmark are ignored.
int g_expected_eye = 0;
bool g_dispatched[2] = {false, false};
const uintptr_t RVA_PYI_OM = 0x1559AF0;
void update_stereo() {
    if (!g_mod_base) return;
    uintptr_t sim = deref(deref(g_mod_base, RVA_PYI_OM), 0x58);
    uintptr_t acm = deref(sim, 392);
    if (sim < 0x10000 || acm < 0x10000) return;
    int mode = *reinterpret_cast<int *>(acm + 0x120);      // 0 = cockpit
    int focused = *reinterpret_cast<int *>(sim + 544);     // 0 = drivable car
    bool st = (mode == 0 && focused == 0);
    if (st != g_stereo)
        acre_log("  cam: %s", st ? "STEREO (drivable-car cockpit) — per-eye DLAA"
                                 : "MONO (spectate/external view) — second pass skipped");
    g_stereo = st;
}
bool g_in_scene = false;                    // scene colour currently bound as the render target
int g_eye_counter = 0;                       // 0 then 1 within a frame
uintptr_t g_cam = 0;
volatile LONG g_inflight = 0;               // guard against re-entrancy
bool g_shadow_valid = false;                 // jitter CB built for current scene (fwd)
void advance_jitter();                       // defined in the jitter section
extern int g_jitter_on;                      // ditto
extern volatile LONG g_tf_seen, g_tf_miss;   // jitter-coverage census, ditto
extern int g_tf_nptr;

// Frame clock: reset the eye counter off the Present count, which works on any headset.
// (Detecting frame start by mirror texture width only worked on the null driver.)
long g_last_frame = -1;
volatile LONG g_sub_n[2] = {0, 0};   // shadow-CB substitutions per eye, this frame
void frame_tick(ID3D11DeviceContext *ctx) {
    long pc = acre_present_count();
    if (pc != g_last_frame) {
        // previous frame's post-processing has landed in renderEye by now
        if (acre_cap_active() && g_om_dev && g_eye_tex[0] && g_eye_tex[1])
            acre_cap_eyes(g_om_dev, ctx, (ID3D11Texture2D *)g_eye_tex[0],
                          (ID3D11Texture2D *)g_eye_tex[1]);
        // Under SPS both eyes render in one pass, so every jitter substitution is counted
        // under eye 0 (g_sub_n[1] stays 0) — that's correct, not the per-eye asymmetry this
        // diag hunts for. Skip it; the wide path applies one jittered CB to both eyes.
        if (g_last_frame >= 0 && g_dispatched[0] && g_jitter_on == 1 && !g_scene_wide)
            acre_diag_jitter_subs((int)g_sub_n[0], (int)g_sub_n[1]);
        acre_cap_endframe();        // advances the capture pair state (mono and stereo)
        g_sub_n[0] = g_sub_n[1] = 0;
        g_snap_done[0] = g_snap_done[1] = false;
        g_expected_eye = 0;
        g_dispatched[0] = g_dispatched[1] = false;
        g_last_frame = pc; g_eye_counter = 0; advance_jitter();
        update_stereo();
#ifdef ACRE_RESEARCH
        static long jf = 0;
        if (g_jitter_on == 1 && (++jf % 300) == 0) {
            acre_log("  jitcov: transform-CB binds=%ld unsubstituted=%ld (%.1f%%) "
                     "distinct CB objects=%d",
                     (long)g_tf_seen, (long)g_tf_miss,
                     g_tf_seen ? 100.0 * g_tf_miss / g_tf_seen : 0.0, g_tf_nptr);
            InterlockedExchange(&g_tf_seen, 0);
            InterlockedExchange(&g_tf_miss, 0);
        }
#endif
    }
}

typedef void (STDMETHODCALLTYPE *PSSRV_t)(ID3D11DeviceContext *, UINT, UINT,
                                          ID3D11ShaderResourceView *const *);
PSSRV_t g_orig_pssrv = nullptr;

ID3D11Resource *srv_resource(ID3D11ShaderResourceView *srv) {
    if (!srv) return nullptr;
    ID3D11Resource *r = nullptr; srv->GetResource(&r);
    if (r) r->Release();
    return r;
}

ID3D11Texture2D *tex_of_rtv(ID3D11RenderTargetView *rtv) {
    if (!rtv) return nullptr;
    ID3D11Resource *r = nullptr; rtv->GetResource(&r);
    if (!r) return nullptr;
    ID3D11Texture2D *t = nullptr;
    r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t);
    r->Release();
    if (t) t->Release();     // borrow identity only
    return t;
}
ID3D11Texture2D *tex_of_dsv(ID3D11DepthStencilView *dsv) {
    if (!dsv) return nullptr;
    ID3D11Resource *r = nullptr; dsv->GetResource(&r);
    if (!r) return nullptr;
    ID3D11Texture2D *t = nullptr;
    r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t);
    r->Release();
    if (t) t->Release();
    return t;
}

void *rtv_resource(ID3D11RenderTargetView *rtv) {
    if (!rtv) return nullptr;
    ID3D11Resource *r = nullptr; rtv->GetResource(&r);
    if (r) r->Release();
    return r;
}

// SPS probe: state + entry defined below (before hkVSCB), forward-declared for hkOM.
extern bool g_sps_scene;
extern bool g_sps_scene_logged;
bool sps_probe_on();

void STDMETHODCALLTYPE hkOM(ID3D11DeviceContext *ctx, UINT n,
                            ID3D11RenderTargetView *const *rtvs,
                            ID3D11DepthStencilView *dsv) {
    LONG seq = InterlockedIncrement(&g_seq);
    // re-read each call so the ini hot-reload works
    // ldr: DLAA at the submit point on the post-tonemap eye image (CSP's arrangement)
    // rather than in-place on the HDR scene. Reuses the submit path wholesale -- it
    // already needs this eye's depth snapshotted before the next eye overwrites it.
    int mode = acre_cfg_mode();
    int ldr = acre_cfg_ldr();
    g_upscale_mode = (mode == 2 || (mode == 1 && ldr)) ? 1 : 0;
    g_dlss_enabled = (mode == 1 && !ldr);

    // ---- Track scene target + frame/eye landmarks (both DLAA and upscale modes; also
    // with mode=off while a capture is active, for no-DLAA reference frames) ----
    if ((g_dlss_enabled || g_upscale_mode || acre_cap_active()) &&
        !InterlockedCompareExchange(&g_inflight, 0, 0)) {
        if (n > 0 && rtvs) {
            frame_tick(ctx);                       // reset eye counter once per frame (Present clock)
            ID3D11Texture2D *ct = tex_of_rtv(rtvs[0]);
            ID3D11Texture2D *dt = tex_of_dsv(dsv);
            // renderEye[0] bound as RT = eye 0's tonemap = eye 0 fully composed.
            // Advance to eye 1 and discard any pending mid-PP scene rebind.
            if (ct && ct == (ID3D11Texture2D *)g_eye_tex[0] && g_expected_eye == 0 &&
                g_dispatched[0]) {
                g_expected_eye = 1;
                g_scene_dirty = false;
            }
            g_eye_counter = g_expected_eye;
            if (ct && dt && g_cam) {
                D3D11_TEXTURE2D_DESC cd; ct->GetDesc(&cd);
                // scene target = HDR colour at exactly the eye render size. anything looser
                // also catches 512x512 reflection probes
                int rw = *reinterpret_cast<int *>(g_cam + 3944);
                int rh = *reinterpret_cast<int *>(g_cam + 3948);
                // per-eye (SPS off): width == renderWidth. double-wide (Single Pass Stereo):
                // width == 2*renderWidth, both eyes side-by-side in one target.
                bool pereye = rw > 0 && (int)cd.Width == rw && (int)cd.Height == rh;
                bool wide   = rw > 0 && (int)cd.Width == 2 * rw && (int)cd.Height == rh;
                if (cd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && (pereye || wide)) {
                    g_scene_color = ct; g_scene_depth = dt; g_scene_wide = wide;
                    // under SPS the single pass does both eyes, so arm on the frame's
                    // wide-dispatch flag (g_dispatched[0]) rather than the per-eye counter.
                    int guard_eye = wide ? 0 : g_expected_eye;
                    // Only arm dirty/rebuild-shadow while THIS eye hasn't dispatched yet.
                    // CSP rebinds the scene texture repeatedly during post-processing; once
                    // the eye has genuinely dispatched, nothing clears g_scene_dirty again
                    // (the PSSRV dispatch path now refuses a second run for the same eye),
                    // so leaving this unconditional left g_shadow_valid=false for the rest
                    // of that eye's draws -- every subsequent VSSetConstantBuffers call
                    // re-ran the jitter shadow-CB rebuild (2 CopyResource + a CS dispatch +
                    // 6 state get/sets), and trusted whatever 320-byte buffer sat at the
                    // cached VS slot as "the transform CB" without re-validating it. Tanked
                    // framerate and could jitter/corrupt unrelated draws (shadow maps
                    // included) for however long that window stayed open.
                    if (!g_dispatched[guard_eye]) {
                        g_scene_dirty = true;
                        g_shadow_valid = false;    // rebuild jittered CB for this eye
                    }
                    acre_diag_scene(cd.Width, cd.Height, cd.SampleDesc.Count, cd.ArraySize);
                }
            }
            // only substitute the jittered CB while the scene target is bound. other passes
            // (Real Mirrors, probes, shadowmaps) reuse the same CB object with their own
            // matrices, and feeding them ours renders them from the wrong camera
            g_in_scene = (g_scene_color && ct == g_scene_color);
        } else {
            g_in_scene = false;
        }
    }

    // ---- SPS investigation: independent double-wide scene detection ----------------
    // Runs regardless of mode (needs no DLSS). The SPS scene target is a big RGBA16F
    // colour + same-size depth, single slice, wider than one eye — which is exactly what
    // the shipping matcher above rejects (it wants width == renderWidth). Setting
    // g_sps_scene here arms the CB dump in hkVSCB for the draws of this pass.
    if (sps_probe_on() && g_cam) {
        g_sps_scene = false;
        if (n > 0 && rtvs) {
            ID3D11Texture2D *sct = tex_of_rtv(rtvs[0]);
            ID3D11Texture2D *sdt = tex_of_dsv(dsv);
            if (sct && sdt) {
                D3D11_TEXTURE2D_DESC cd, dd; sct->GetDesc(&cd); sdt->GetDesc(&dd);
                int rw = *reinterpret_cast<int *>(g_cam + 3944);
                int rh = *reinterpret_cast<int *>(g_cam + 3948);
                if (cd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && cd.ArraySize == 1 &&
                    cd.Width >= 2048 && dd.Width == cd.Width && dd.Height == cd.Height) {
                    g_sps_scene = true;
                    if (!g_sps_scene_logged) {
                        acre_log("  SPS: double-wide scene target %ux%u  renderWidth=%d "
                                 "renderHeight=%d  width/rw=%.2f", cd.Width, cd.Height, rw, rh,
                                 rw > 0 ? (float)cd.Width / rw : 0.f);
                        g_sps_scene_logged = true;
                    }
                }
            }
        }
    }

#ifdef ACRE_RESEARCH
    // Motion-vector target hunt. CSP's monitor-mode DLSS is fed an R16G16_FLOAT texture
    // with bind=0x28 (RENDER_TARGET|SHADER_RESOURCE), i.e. it is rendered, so it passes
    // through here. If the same shape appears in VR, CSP is producing real per-object
    // motion vectors we could consume instead of synthesising camera-only ones from
    // depth -- which is the ceiling on our current quality.
    // Research-only: this costs a describe_rtv() on every render-target bind.
    if (n > 0 && rtvs) {
        static unsigned seen_w[8], seen_h[8];
        static int seen_n = 0;
        for (UINT i = 0; i < n; i++) {
            unsigned w = 0, h = 0, f = 0;
            describe_rtv(rtvs[i], w, h, f);
            if (f != DXGI_FORMAT_R16G16_FLOAT || w < 512) continue;
            bool known = false;
            for (int k = 0; k < seen_n; k++)
                if (seen_w[k] == w && seen_h[k] == h) { known = true; break; }
            if (!known && seen_n < 8) {
                seen_w[seen_n] = w; seen_h[seen_n] = h; seen_n++;
                acre_log("  mvhunt: R16G16_FLOAT RENDER TARGET bound %ux%u slot=%u res=%p "
                         "— candidate motion-vector buffer", w, h, i, res_of_rtv(rtvs[i]));
            }
        }
    }
#endif  // ACRE_RESEARCH

    if (n > 0 || dsv) {
        Sig s = {};
        if (n > 0 && rtvs) describe_rtv(rtvs[0], s.rw, s.rh, s.rf, &s.ra, &s.rs);
        describe_dsv(dsv, s.dw, s.dh, s.df, &s.da, &s.ds);
        // first-frames logging so the log shows what CSP binds
        bool key = (s.rf == DXGI_FORMAT_R16G16B16A16_FLOAT && s.dw >= 512 &&
                    s.rw == s.dw && s.rh == s.dh);
        if (key && g_pereye_logged < 12) {
            InterlockedIncrement(&g_pereye_logged);
            acre_log("  OM* seq=%ld PER-EYE %ux%u arr=%u smp=%u color=%p depth=%p",
                     seq, s.rw, s.rh, s.ra, s.rs,
                     n > 0 ? res_of_rtv(rtvs[0]) : nullptr, res_of_dsv(dsv));
        } else if (s.rw == 2560 && g_pereye_logged < 12) {
            acre_log("  OM* seq=%ld  [mirror 2560x1440 — frame landmark]", seq);
        } else if (g_logged < 20 && (s.rw >= 512 || s.dw >= 512) && !seen(s)) {
            InterlockedIncrement(&g_logged);
            acre_log("  OM: color=%ux%u %s arr=%u smp=%u | depth=%ux%u %s arr=%u smp=%u",
                     s.rw, s.rh, fmtname(s.rf), s.ra, s.rs,
                     s.dw, s.dh, fmtname(s.df), s.da, s.ds);
        }
    }
    g_orig_om(ctx, n, rtvs, dsv);
}

// ---- Jitter injection into CSP's transform CB (320 bytes: view@0, projection@64) ----
// Copy CSP's CB to a shadow, add sub-pixel jitter to the frustum offset (bytes 72/88)
// with a tiny CS, bind the shadow instead. All GPU-side, no readback.
typedef void (STDMETHODCALLTYPE *VSCB_t)(ID3D11DeviceContext *, UINT, UINT,
                                         ID3D11Buffer *const *);
VSCB_t g_orig_vscb = nullptr;
int g_jitter_on = -1;
ID3D11Buffer *g_cb_staging = nullptr;        // one-time identification readback only
ID3D11Buffer *g_shadow_cb = nullptr;         // DEFAULT CB: our jittered copy
ID3D11Buffer *g_work = nullptr;              // DEFAULT 320B raw UAV: CS scratch
ID3D11UnorderedAccessView *g_work_uav = nullptr;
ID3D11Buffer *g_jparam = nullptr;            // 16B CB: NDC jitter offset for the CS
ID3D11ComputeShader *g_jitter_cs = nullptr;  // adds jitter to live CB contents (jitter_add.hlsl)
ID3D11Buffer *g_transform_ptr = nullptr;     // the exact CSP CB to substitute this scene
int  g_transform_slot = -1;                  // VS slot of the transform CB (found once)
bool g_transform_found = false;
// CSP's actual scene near/far, read from the transform CB ([52]/[53]) when it's found.
// The SPS reprojection MUST rebuild the per-eye projection with THESE (the values the depth
// was rendered with) — they vary by config (0.05/15000 or 0.1/20000 seen), and a mismatch
// puts reconstructed world at the wrong distance → wrong MVs → the whole scene shakes.
float g_cb_near = 0.0f, g_cb_far = 0.0f;
float g_jitter_px[2] = {0, 0};               // this frame's jitter, pixels
unsigned g_jframe = 0;

// Jitter COVERAGE diagnostic. g_transform_ptr is latched when the shadow is built (once
// per eye). If CSP rotates among several transform-CB objects, later draws bind a
// different pointer, miss the substitution, and render UNJITTERED while the rest of the
// scene is jittered -- DLSS then rocks those elements back and forth between sample
// positions. Counts binds at the transform slot vs. how many actually got substituted.
volatile LONG g_tf_seen = 0, g_tf_miss = 0;
void *g_tf_ptrs[8];
int g_tf_nptr = 0;

float halton(unsigned i, unsigned b) {
    float f = 1, r = 0;
    while (i > 0) { f /= b; r += f * (i % b); i /= b; }
    return r;
}
void advance_jitter() {
    g_jframe++;
    // Scale amplitude (hot-reloadable). Scaling here covers BOTH the render offset (built from
    // g_jitter_px) and the value reported to DLSS (acre_get_jitter reads g_jitter_px), so they
    // stay consistent. Lower = smaller sub-pixel shift = less grazing-edge crawl, less AA detail.
    float s = acre_cfg_jitter_scale();
    g_jitter_px[0] = (halton(g_jframe % 64 + 1, 2) - 0.5f) * s;   // [-0.5, 0.5]*s px
    g_jitter_px[1] = (halton(g_jframe % 64 + 1, 3) - 0.5f) * s;
}
bool is_transform_cb(const float *f) {   // projection at [16]: M[2]=(0,0,-1,-near)
    return f[24] > -0.05f && f[24] < 0.05f && f[26] < -0.9f && f[26] > -1.1f &&
           f[27] < -0.01f && f[27] > -0.5f;
}

void ensure_shadow_bufs(ID3D11Device *dev) {
    if (!g_shadow_cb) {
        D3D11_BUFFER_DESC d = {}; d.ByteWidth = 320;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&d, nullptr, &g_shadow_cb);
    }
    if (!g_work) {
        D3D11_BUFFER_DESC d = {}; d.ByteWidth = 320;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (SUCCEEDED(dev->CreateBuffer(&d, nullptr, &g_work)) && g_work) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = 80;                    // 320 bytes / 4
            ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            dev->CreateUnorderedAccessView(g_work, &ud, &g_work_uav);
        }
    }
    if (!g_jparam) {
        D3D11_BUFFER_DESC d = {}; d.ByteWidth = 16;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&d, nullptr, &g_jparam);
    }
    if (!g_jitter_cs)
        dev->CreateComputeShader(g_jitter_add_cs, sizeof(g_jitter_add_cs), nullptr, &g_jitter_cs);
}

// Build the jittered shadow from the live contents of CSP's CB: copy to a raw scratch
// buffer, 1-thread CS adds the NDC jitter to the frustum offsets, copy into the shadow CB.
// Adding to live values means FOV and per-eye frustum always survive intact.
void build_shadow_gpu(ID3D11DeviceContext *ctx, ID3D11Buffer *src) {
    if (!g_work || !g_work_uav || !g_jparam || !g_jitter_cs) return;
    float w = g_cam ? (float)*reinterpret_cast<int *>(g_cam + 3944) : 1524.f;
    float h = g_cam ? (float)*reinterpret_cast<int *>(g_cam + 3948) : 1696.f;
    if (w < 1) w = 1524.f; if (h < 1) h = 1696.f;
    float ndc[4] = {-2.0f * g_jitter_px[0] / w, -2.0f * g_jitter_px[1] / h, 0, 0};
    ctx->UpdateSubresource(g_jparam, 0, nullptr, ndc, 0, 0);
    ctx->CopyResource(g_work, src);

    // save/restore CS state we touch
    ID3D11ComputeShader *oldCS = nullptr;
    ID3D11Buffer *oldCB = nullptr;
    ID3D11UnorderedAccessView *oldUAV = nullptr;
    ctx->CSGetShader(&oldCS, nullptr, nullptr);
    ctx->CSGetConstantBuffers(0, 1, &oldCB);
    ctx->CSGetUnorderedAccessViews(0, 1, &oldUAV);

    ctx->CSSetShader(g_jitter_cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_jparam);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_work_uav, nullptr);
    ctx->Dispatch(1, 1, 1);

    ctx->CSSetShader(oldCS, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &oldCB);
    ctx->CSSetUnorderedAccessViews(0, 1, &oldUAV, nullptr);
    if (oldCS) oldCS->Release();
    if (oldCB) oldCB->Release();
    if (oldUAV) oldUAV->Release();

    ctx->CopyResource(g_shadow_cb, g_work);
    g_shadow_valid = true;
}

// ---- SPS investigation (env ACRE_SPS_PROBE=1) ---------------------------------------
// Under Single Pass Stereo the scene renders to one double-wide RGBA16F target; the
// shipping per-eye matcher rejects it, so jitter/DLAA are inert. To re-establish jitter
// we need the transform-CB layout under SPS, which now likely holds BOTH eyes' matrices.
// This detects the double-wide scene target and dumps each distinct VS constant buffer
// bound while it is active, so the layout can be read from acre_proxy.log. Temporary
// diagnostic — the readback stall is fine for a non-perf run. Gated off in shipping.
int  g_sps_probe = -1;                        // -1 unknown, 0 off, 1 on (env, read once)
bool g_sps_scene = false;                     // double-wide scene target currently bound
bool g_sps_scene_logged = false;
int  g_sps_dumped = 0;
struct SpsCbSeen { void *ptr; UINT slot; UINT size; };
SpsCbSeen g_sps_seen[64];
int g_sps_seen_n = 0;

bool sps_probe_on() {
    if (g_sps_probe < 0) {
        char v[8] = {0};
        g_sps_probe = (GetEnvironmentVariableA("ACRE_SPS_PROBE", v, sizeof(v)) && atoi(v)) ? 1 : 0;
        if (g_sps_probe) acre_log("  SPS: probe ENABLED (ACRE_SPS_PROBE) — CB layout dump");
    }
    return g_sps_probe == 1;
}

// The projection block CSP uses everywhere: M[0]=(1,0,·,0) M[1]=(0,1,·,0)
// M[2]=(0,0,-1,-near) — FOV applied outside, so [·] at row0/row1 col2 is the per-eye NDC
// offset (0 = mono/centered). Returns the float offset where it starts, or -1.
int sps_find_proj(const float *f, UINT nf) {
    for (UINT o = 0; o + 15 < nf; o += 4) {          // 16-float (row) aligned
        if (f[o+0] > 0.99f && f[o+0] < 1.01f && f[o+1] == 0.f && f[o+3] == 0.f &&
            f[o+4] == 0.f && f[o+5] > 0.99f && f[o+5] < 1.01f && f[o+7] == 0.f &&
            f[o+10] < -0.9f && f[o+10] > -1.1f &&                 // M[2][2] = -1
            f[o+11] < -0.01f && f[o+11] > -0.5f)                  // M[2][3] = -near
            return (int)o;
    }
    return -1;
}

void sps_dump_cb(ID3D11DeviceContext *ctx, ID3D11Buffer *b, UINT slot) {
    if (!b || !g_om_dev || g_sps_dumped >= 48) return;
    D3D11_BUFFER_DESC bd; b->GetDesc(&bd);
    if (bd.ByteWidth < 64 || bd.ByteWidth > 1024) return;      // camera CBs are small
    for (int i = 0; i < g_sps_seen_n; i++)
        if (g_sps_seen[i].ptr == b && g_sps_seen[i].slot == slot && g_sps_seen[i].size == bd.ByteWidth)
            return;                                            // mapped this one already
    if (g_sps_seen_n < 64) g_sps_seen[g_sps_seen_n++] = SpsCbSeen{ b, slot, bd.ByteWidth };

    D3D11_BUFFER_DESC sd = {}; sd.ByteWidth = bd.ByteWidth;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer *st = nullptr;
    if (FAILED(g_om_dev->CreateBuffer(&sd, nullptr, &st)) || !st) return;
    ctx->CopyResource(st, b);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const float *f = static_cast<const float *>(m.pData);
        UINT nf = bd.ByteWidth / 4;
        int po = sps_find_proj(f, nf);                          // only log projection CBs
        if (po >= 0) {
            UINT lf = nf > 96 ? 96 : nf;
            acre_log("  SPScb: slot=%u size=%u bytes  PROJECTION at float[%d]  "
                     "eyeOffX=%.4f eyeOffY=%.4f", slot, bd.ByteWidth, po, f[po+2], f[po+6]);
            for (UINT o = 0; o + 3 < lf; o += 4)
                acre_log("    [%2u] % .4f % .4f % .4f % .4f", o, f[o], f[o+1], f[o+2], f[o+3]);
            g_sps_dumped++;
        }
        ctx->Unmap(st, 0);
    }
    st->Release();
}

void STDMETHODCALLTYPE hkVSCB(ID3D11DeviceContext *ctx, UINT start, UINT num,
                              ID3D11Buffer *const *bufs) {
    g_jitter_on = acre_cfg_jitter();   // hot-reloadable

    if (sps_probe_on() && g_sps_scene && bufs && num > 0) {
        for (UINT i = 0; i < num; i++)
            if (bufs[i]) sps_dump_cb(ctx, bufs[i], start + i);
    }

    // per-eye view snapshot: first VSCB of this eye's scene pass, after viveRenderPass
    // has written cam+2512 for this eye and before it moves on
    if (g_in_scene && g_scene_dirty && g_cam) {
        if (g_eye_counter < 2 && !g_snap_done[g_eye_counter]) {
            memcpy(g_eye_view[g_eye_counter], reinterpret_cast<void *>(g_cam + 2512), 64);
            g_snap_done[g_eye_counter] = true;
            InterlockedOr(&g_eye_view_valid, 1 << g_eye_counter);
        } else if (g_scene_wide && g_snap_done[0]) {
            // SPS is a single pass with no second-eye counter, but CSP renders eye 0 first
            // then eye 1, rewriting cam+2512 per eye. Keep the LATEST scene-bind view: by the
            // last bind it is eye 1's render-time view — more reliable than reading cam+2512
            // at the post-render dispatch point, where head-tracking may have moved it.
            memcpy(g_eye_view[1], reinterpret_cast<void *>(g_cam + 2512), 64);
            InterlockedOr(&g_eye_view_valid, 2);
        }
    }

    // Non-SPS: build the jittered shadow ONCE per scene (per eye pass), cached until the
    // next eye/frame. SPS: CSP renders both eyes in ONE pass, reusing the SAME transform-CB
    // pointer but rewriting its contents (per-eye projection) between the two eye draws — so
    // a cached shadow built from eye 0 would be substituted for eye 1 too and collapse the
    // stereo. Rebuild the shadow from the CB's LIVE content on every bind during the scene
    // pass, so each eye's draws get their own projection + the same sub-pixel jitter.
    bool jbuild = g_jitter_on && g_in_scene && g_scene_dirty && bufs && g_om_dev &&
                  !InterlockedCompareExchange(&g_inflight, 0, 0) &&
                  (g_scene_wide || !g_shadow_valid);
    if (jbuild) {
        InterlockedExchange(&g_inflight, 1);
        ensure_shadow_bufs(g_om_dev);
        ID3D11Buffer *tcb = nullptr;

        if (g_transform_found) {
            int idx = g_transform_slot - (int)start;
            if (idx >= 0 && idx < (int)num && bufs[idx]) {
                D3D11_BUFFER_DESC bd; bufs[idx]->GetDesc(&bd);
                if (bd.ByteWidth == 320) tcb = bufs[idx];
            }
        } else {
            // one-time: find which 320B CB holds the camera by its projection signature
            for (UINT i = 0; i < num && !tcb; i++) {
                if (!bufs[i]) continue;
                D3D11_BUFFER_DESC bd; bufs[i]->GetDesc(&bd);
                if (bd.ByteWidth != 320) continue;
                if (!g_cb_staging) {
                    D3D11_BUFFER_DESC sd = {}; sd.ByteWidth = 320;
                    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (FAILED(g_om_dev->CreateBuffer(&sd, nullptr, &g_cb_staging))) { g_cb_staging = nullptr; break; }
                }
                ctx->CopyResource(g_cb_staging, bufs[i]);
                D3D11_MAPPED_SUBRESOURCE rm;
                if (FAILED(ctx->Map(g_cb_staging, 0, D3D11_MAP_READ, 0, &rm))) break;
                float cbcopy[80];
                memcpy(cbcopy, rm.pData, sizeof(cbcopy));
                bool match = is_transform_cb((const float *)rm.pData);
                ctx->Unmap(g_cb_staging, 0);
                if (match) {
                    acre_diag_matrices(cbcopy, g_cam);
                    // CSP CB layout: [48..51]=camPos, [52]=near, [53]=far. Capture the real
                    // near/far so the SPS reprojection projection matches the rendered depth.
                    if (cbcopy[52] > 0.0f && cbcopy[52] < 5.0f) g_cb_near = cbcopy[52];
                    if (cbcopy[53] > 100.0f) g_cb_far = cbcopy[53];
                    g_transform_slot = (int)(start + i); g_transform_found = true; tcb = bufs[i];
                    acre_log("  jitter: transform CB at VS slot %d (live-content CS add, GPU-side); "
                             "scene near=%.4f far=%.1f", g_transform_slot, g_cb_near, g_cb_far);
                }
            }
        }

        if (tcb) { g_transform_ptr = tcb; build_shadow_gpu(ctx, tcb); }

        InterlockedExchange(&g_inflight, 0);
    }

#ifdef ACRE_RESEARCH
    // Coverage census: is every transform-CB bind actually getting the jittered shadow?
    // Research-only -- it calls GetDesc() once per constant-buffer bind, i.e. per draw.
    if (g_jitter_on && g_in_scene && g_transform_found && bufs) {
        int idx = g_transform_slot - (int)start;
        if (idx >= 0 && idx < (int)num && bufs[idx]) {
            D3D11_BUFFER_DESC bd; bufs[idx]->GetDesc(&bd);
            if (bd.ByteWidth == 320) {
                InterlockedIncrement(&g_tf_seen);
                if (bufs[idx] != g_transform_ptr) InterlockedIncrement(&g_tf_miss);
                bool known = false;
                for (int i = 0; i < g_tf_nptr; i++)
                    if (g_tf_ptrs[i] == bufs[idx]) { known = true; break; }
                if (!known && g_tf_nptr < 8) g_tf_ptrs[g_tf_nptr++] = bufs[idx];
            }
        }
    }
#endif  // ACRE_RESEARCH

    // swap in the shadow whenever CSP binds its transform CB, scene pass only
    if (g_jitter_on && g_in_scene && g_shadow_valid && g_shadow_cb && bufs && num > 0 &&
        !InterlockedCompareExchange(&g_inflight, 0, 0)) {
        ID3D11Buffer *local[16];
        if (num <= 16) {
            bool sub = false;
            for (UINT i = 0; i < num; i++) {
                local[i] = bufs[i];
                if (bufs[i] == g_transform_ptr) { local[i] = g_shadow_cb; sub = true; }
            }
            if (sub) {
                InterlockedIncrement(&g_sub_n[g_eye_counter <= 0 ? 0 : 1]);
                g_orig_vscb(ctx, start, num, local); return;
            }
        }
    }
    g_orig_vscb(ctx, start, num, bufs);
}

extern "C" const float *acre_eye_view(int eye) {
    if (eye < 0 || eye > 1 || !(g_eye_view_valid & (1 << eye))) return nullptr;
    return g_eye_view[eye];
}
extern "C" void acre_get_jitter(float *jx, float *jy) { *jx = g_jitter_px[0]; *jy = g_jitter_px[1]; }
extern "C" uintptr_t acre_get_cam(void) { return g_cam; }
extern "C" ID3D11Device *acre_get_device(void) { return g_om_dev; }

// Scene complete = CSP first binds the scene colour as a PS input for post-processing.
// dlaa: run DLSS in place here. dlss: just snapshot this eye's depth for the submit
// upscale (the shared depth gets overwritten by the next eye).
void STDMETHODCALLTYPE hkPSSRV(ID3D11DeviceContext *ctx, UINT start, UINT num,
                               ID3D11ShaderResourceView *const *srvs) {
    if ((g_dlss_enabled || g_upscale_mode || acre_cap_active()) && g_scene_dirty &&
        g_scene_color && g_om_dev &&
        g_cam && !InterlockedCompareExchange(&g_inflight, 0, 0) && srvs) {
        bool hit = false;
        for (UINT i = 0; i < num && !hit; i++)
            if (srv_resource(srvs[i]) == (ID3D11Resource *)g_scene_color) hit = true;
        if (hit && g_scene_wide && !g_dispatched[0]) {
            // ---- Single Pass Stereo: both eyes live in one double-wide target, produced
            // in a single pass, so process them together at this one injection point. The
            // two-pass eye-pairing landmarks (g_expected_eye / renderEye binds) don't apply.
            g_scene_dirty = false;
            g_dispatched[0] = g_dispatched[1] = true;
            InterlockedExchange(&g_inflight, 1);
            __try {
                if (g_dlss_enabled) {
                    // ldr=0: DLAA in place on the double-wide HDR scene.
                    acre_diag_eye(0); acre_diag_eye(1);
                    D3D11StateBackup bak;
                    d3d11_backup(ctx, &bak);
                    ctx->OMSetRenderTargets(0, nullptr, nullptr);
                    acre_dlss_inplace_sps(g_om_dev, ctx, g_scene_color, g_scene_depth, g_cam);
                    d3d11_restore(ctx, &bak);
                } else if (g_upscale_mode) {
                    // ldr=1: DLAA happens later at the submit point on each eye's LDR renderEye.
                    // Here just extract both eyes' depth from the double-wide for that path's MVs.
                    D3D11StateBackup bak;
                    d3d11_backup(ctx, &bak);
                    acre_sps_capture_depth_submit(g_om_dev, ctx, g_scene_color, g_scene_depth, g_cam);
                    d3d11_restore(ctx, &bak);
                }
                // (mode=off reference capture under SPS: not wired — DLAA is the target.)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                static long e = 0;
                if (e++ < 3) acre_log("  sps: SEH 0x%08lx — disabled", GetExceptionCode());
                g_dlss_enabled = false;
            }
            InterlockedExchange(&g_inflight, 0);
        } else if (hit && !g_scene_wide && !g_dispatched[g_expected_eye]) {
            int eye = g_expected_eye;
            g_scene_dirty = false;
            g_dispatched[eye] = true;
            if (!g_stereo && eye == 1)
                goto mono_skip;     // no second eye exists; this is eye 0's image again
            acre_diag_eye(eye);
            InterlockedExchange(&g_inflight, 1);
            __try {
                if (!g_dlss_enabled && !g_upscale_mode) {
                    // mode=off capture: dump the raw scene as the reference image
                    acre_cap_in(g_om_dev, ctx, eye, g_scene_color);
                } else if (g_upscale_mode) {
                    acre_up_capture_depth(ctx, g_scene_depth, eye);   // for submit upscale
                } else {
                    D3D11StateBackup bak;
                    d3d11_backup(ctx, &bak);
                    ctx->OMSetRenderTargets(0, nullptr, nullptr);
                    acre_dlss_inplace(g_om_dev, ctx, g_scene_color, g_scene_depth, eye, g_cam);
                    d3d11_restore(ctx, &bak);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                static long e = 0;
                if (e++ < 3) acre_log("  ip: SEH 0x%08lx — disabled", GetExceptionCode());
                g_dlss_enabled = false;
            }
            InterlockedExchange(&g_inflight, 0);
mono_skip:;
        }
    }
    g_orig_pssrv(ctx, start, num, srvs);
}

void *patch_slot(void **vtable, int i, void *repl) {
    DWORD old;
    if (!VirtualProtect(&vtable[i], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    void *prev = vtable[i]; vtable[i] = repl;
    VirtualProtect(&vtable[i], sizeof(void *), old, &old);
    return prev;
}

}  // namespace

static uintptr_t deref(uintptr_t p, uintptr_t off) {
    uintptr_t a = p + off; return (a < 0x10000) ? 0 : *reinterpret_cast<uintptr_t *>(a);
}

// True while CSP's double-wide Single Pass Stereo scene target is the one being processed.
// Lets shared code (dlss_pass diagnostics) skip two-pass-only checks that are meaningless
// under SPS (both eyes share one mono view, so their "separation" is legitimately zero).
extern "C" int acre_sps_active(void) { return g_scene_wide ? 1 : 0; }

// CSP's live scene near/far (from the transform CB). Returns false until captured.
extern "C" int acre_scene_nearfar(float *nearz, float *farz) {
    if (g_cb_near <= 0.0f || g_cb_far <= 0.0f) return 0;
    *nearz = g_cb_near; *farz = g_cb_far; return 1;
}

extern "C" void acre_install_om_hook(ID3D11DeviceContext *ctx) {
    if (g_orig_om) return;

    // grab the live camera + log its render targets for reference
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    g_mod_base = base;
    ctx->GetDevice(&g_om_dev);            // for creating our resources
    // The camera walk resolves a StereoCameraVive. In monitor mode those pointers are
    // non-null but not that type, so reading viveData off them access-violates and the
    // vtable hooks below never got installed (the caller's SEH guard swallowed it, so it
    // looked like "no data" rather than a crash). Guard the walk; hooks install either way.
    __try {
    uintptr_t pyi = deref(base, 0x1559AF0);
    if (pyi) {
        uintptr_t cam = deref(deref(deref(pyi, 0x58), 392), 280);
        g_cam = cam;
        if (cam) {
            uintptr_t colTex = deref(deref(deref(cam, 5088), 24), 0);   // rtYebisResolved
            uintptr_t depTex = deref(deref(deref(cam, 5096), 24), 0);   // rtYebisResolvedDepth
            acre_log("  OM: vanilla rtYebisResolved tex=%p  rtYebisResolvedDepth tex=%p",
                     (void *)colTex, (void *)depTex);
            for (int eye = 0; eye < 2; eye++) {
                g_eye_tex[eye] = reinterpret_cast<void *>(
                    deref(deref(deref(cam, 1968 + eye * 176), 24), 0));  // viveData[eye].renderEye
                acre_log("  OM: viveData[%d].renderEye tex=%p", eye, g_eye_tex[eye]);
            }
        }
    }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_cam = 0;
        acre_log("  OM: no VR camera (monitor mode?) — hooks still installed for spying");
    }

    void **vt = *reinterpret_cast<void ***>(ctx);
    g_orig_om = reinterpret_cast<OMSet_t>(vt[33]);      // OMSetRenderTargets
    void *prev = patch_slot(vt, 33, reinterpret_cast<void *>(&hkOM));
    g_orig_pssrv = reinterpret_cast<PSSRV_t>(vt[8]);    // PSSetShaderResources
    void *prev2 = patch_slot(vt, 8, reinterpret_cast<void *>(&hkPSSRV));
    g_orig_vscb = reinterpret_cast<VSCB_t>(vt[7]);      // VSSetConstantBuffers (proj hunt)
    void *prev3 = patch_slot(vt, 7, reinterpret_cast<void *>(&hkVSCB));
    acre_log("  OM: hooks %s/%s/%s", prev ? "ok" : "FAIL", prev2 ? "ok" : "FAIL",
             prev3 ? "ok" : "FAIL");
    acre_diag_hooks(prev && prev2 && prev3);
}
