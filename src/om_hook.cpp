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
extern "C" void acre_up_capture_depth(ID3D11DeviceContext *ctx, ID3D11Texture2D *depth, int eye);
extern "C" int acre_cfg_mode(void);
extern "C" int acre_cfg_jitter(void);
extern "C" long acre_present_count(void);   // frame clock, in dxgi_hook.cpp
extern "C" void acre_diag_hooks(int ok);
extern "C" void acre_diag_scene(unsigned w, unsigned h, unsigned samples, unsigned arraysize);
extern "C" void acre_diag_eye(int eye);
extern "C" void acre_diag_matrices(const float *cb, uintptr_t cam);
extern "C" void acre_diag_jitter_subs(int e0, int e1);
extern "C" int acre_cap_active(void);
extern "C" void acre_cap_in(ID3D11Device *, ID3D11DeviceContext *, int, ID3D11Texture2D *);
extern "C" void acre_cap_mark(int eye);

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
struct Sig { unsigned rw, rh, rf, dw, dh, df; };
Sig g_seen[64];
int g_seen_n = 0;

bool seen(const Sig &s) {
    for (int i = 0; i < g_seen_n; i++)
        if (!memcmp(&g_seen[i], &s, sizeof(Sig))) return true;
    if (g_seen_n < 64) g_seen[g_seen_n++] = s;
    return false;
}

void describe_rtv(ID3D11RenderTargetView *rtv, unsigned &w, unsigned &h, unsigned &f) {
    w = h = f = 0;
    if (!rtv) return;
    ID3D11Resource *res = nullptr;
    rtv->GetResource(&res);
    if (!res) return;
    ID3D11Texture2D *t = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t)) && t) {
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); w = d.Width; h = d.Height; f = d.Format;
        t->Release();
    }
    res->Release();
}
void describe_dsv(ID3D11DepthStencilView *dsv, unsigned &w, unsigned &h, unsigned &f) {
    w = h = f = 0;
    if (!dsv) return;
    ID3D11Resource *res = nullptr;
    dsv->GetResource(&res);
    if (!res) return;
    ID3D11Texture2D *t = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t)) && t) {
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); w = d.Width; h = d.Height; f = d.Format;
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

// Frame clock: reset the eye counter off the Present count, which works on any headset.
// (Detecting frame start by mirror texture width only worked on the null driver.)
long g_last_frame = -1;
volatile LONG g_sub_n[2] = {0, 0};   // shadow-CB substitutions per eye, this frame
void frame_tick() {
    long pc = acre_present_count();
    if (pc != g_last_frame) {
        if (g_last_frame >= 0 && g_dispatched[0] && g_jitter_on == 1)
            acre_diag_jitter_subs((int)g_sub_n[0], (int)g_sub_n[1]);
        if (!g_stereo && g_dispatched[0])
            acre_cap_mark(1);       // mono: single dispatch per frame; close capture frame
        g_sub_n[0] = g_sub_n[1] = 0;
        g_snap_done[0] = g_snap_done[1] = false;
        g_expected_eye = 0;
        g_dispatched[0] = g_dispatched[1] = false;
        g_last_frame = pc; g_eye_counter = 0; advance_jitter();
        update_stereo();
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

void STDMETHODCALLTYPE hkOM(ID3D11DeviceContext *ctx, UINT n,
                            ID3D11RenderTargetView *const *rtvs,
                            ID3D11DepthStencilView *dsv) {
    LONG seq = InterlockedIncrement(&g_seq);
    // re-read each call so the ini hot-reload works
    int mode = acre_cfg_mode();
    g_upscale_mode = (mode == 2) ? 1 : 0;
    g_dlss_enabled = (mode == 1);

    // ---- Track scene target + frame/eye landmarks (both DLAA and upscale modes; also
    // with mode=off while a capture is active, for no-DLAA reference frames) ----
    if ((g_dlss_enabled || g_upscale_mode || acre_cap_active()) &&
        !InterlockedCompareExchange(&g_inflight, 0, 0)) {
        if (n > 0 && rtvs) {
            frame_tick();                          // reset eye counter once per frame (Present clock)
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
                if (cd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && rw > 0 &&
                    (int)cd.Width == rw && (int)cd.Height == rh) {
                    g_scene_color = ct; g_scene_depth = dt; g_scene_dirty = true;
                    g_shadow_valid = false;        // rebuild jittered CB for this eye
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

    if (n > 0 || dsv) {
        Sig s = {};
        if (n > 0 && rtvs) describe_rtv(rtvs[0], s.rw, s.rh, s.rf);
        describe_dsv(dsv, s.dw, s.dh, s.df);
        // first-frames logging so the log shows what CSP binds
        bool key = (s.rf == DXGI_FORMAT_R16G16B16A16_FLOAT && s.dw >= 512 &&
                    s.rw == s.dw && s.rh == s.dh);
        if (key && g_pereye_logged < 12) {
            InterlockedIncrement(&g_pereye_logged);
            acre_log("  OM* seq=%ld PER-EYE %ux%u color=%p depth=%p",
                     seq, s.rw, s.rh,
                     n > 0 ? res_of_rtv(rtvs[0]) : nullptr, res_of_dsv(dsv));
        } else if (s.rw == 2560 && g_pereye_logged < 12) {
            acre_log("  OM* seq=%ld  [mirror 2560x1440 — frame landmark]", seq);
        } else if (g_logged < 20 && (s.rw >= 512 || s.dw >= 512) && !seen(s)) {
            InterlockedIncrement(&g_logged);
            acre_log("  OM: color=%ux%u %s | depth=%ux%u %s",
                     s.rw, s.rh, fmtname(s.rf), s.dw, s.dh, fmtname(s.df));
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
float g_jitter_px[2] = {0, 0};               // this frame's jitter, pixels
unsigned g_jframe = 0;

float halton(unsigned i, unsigned b) {
    float f = 1, r = 0;
    while (i > 0) { f /= b; r += f * (i % b); i /= b; }
    return r;
}
void advance_jitter() {
    g_jframe++;
    g_jitter_px[0] = halton(g_jframe % 64 + 1, 2) - 0.5f;   // [-0.5, 0.5] px
    g_jitter_px[1] = halton(g_jframe % 64 + 1, 3) - 0.5f;
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

void STDMETHODCALLTYPE hkVSCB(ID3D11DeviceContext *ctx, UINT start, UINT num,
                              ID3D11Buffer *const *bufs) {
    g_jitter_on = acre_cfg_jitter();   // hot-reloadable

    // per-eye view snapshot: first VSCB of this eye's scene pass, after viveRenderPass
    // has written cam+2512 for this eye and before it moves on
    if (g_in_scene && g_scene_dirty && g_cam && g_eye_counter < 2 &&
        !g_snap_done[g_eye_counter]) {
        memcpy(g_eye_view[g_eye_counter], reinterpret_cast<void *>(g_cam + 2512), 64);
        g_snap_done[g_eye_counter] = true;
        InterlockedOr(&g_eye_view_valid, 1 << g_eye_counter);
    }

    if (g_jitter_on && g_in_scene && g_scene_dirty && !g_shadow_valid && bufs && g_om_dev &&
        !InterlockedCompareExchange(&g_inflight, 0, 0)) {
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
                    g_transform_slot = (int)(start + i); g_transform_found = true; tcb = bufs[i];
                    acre_log("  jitter: transform CB at VS slot %d (live-content CS add, GPU-side)",
                             g_transform_slot);
                }
            }
        }

        if (tcb) { g_transform_ptr = tcb; build_shadow_gpu(ctx, tcb); }
        InterlockedExchange(&g_inflight, 0);
    }

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
        if (hit && !g_dispatched[g_expected_eye]) {
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
                    acre_cap_mark(eye);
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

extern "C" void acre_install_om_hook(ID3D11DeviceContext *ctx) {
    if (g_orig_om) return;

    // grab the live camera + log its render targets for reference
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    g_mod_base = base;
    ctx->GetDevice(&g_om_dev);            // for creating our resources
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
