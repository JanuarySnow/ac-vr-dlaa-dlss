// SPDX-License-Identifier: GPL-3.0-or-later
// submit_hook intercepts IVRCompositor::Submit (vtable idx 5) for DLSS upscaling.
// AC renders each eye reduced (see res_hook), we upscale renderEye to native and submit
// that instead. Depth comes from om_hook's per-eye snapshots.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include "ctx_backup.h"

extern "C" void acre_log(const char *fmt, ...);
extern "C" bool acre_ngx_create_upscale(ID3D11DeviceContext *, unsigned, unsigned, unsigned, unsigned);
extern "C" void acre_dlss_upscale(ID3D11Device *, ID3D11DeviceContext *, ID3D11Texture2D *,
                                  ID3D11Texture2D *, int, ID3D11Texture2D *, uintptr_t);
extern "C" uintptr_t acre_get_cam(void);
extern "C" float acre_cfg_upscale(void);
extern "C" int  acre_cfg_out_w(void);
extern "C" int  acre_cfg_out_h(void);
extern "C" bool acre_res_native(unsigned *w, unsigned *h);

namespace {

struct VRTextureBounds_t { float uMin, vMin, uMax, vMax; };
struct Texture_t { void *handle; int eType; int eColorSpace; };
typedef int (STDMETHODCALLTYPE *Submit_t)(void *self, int eEye, const Texture_t *pTexture,
                                          const VRTextureBounds_t *pBounds, int nSubmitFlags);

const uintptr_t RVA_COMPOSITOR = 0x155A408;
unsigned g_out_w = 0, g_out_h = 0;             // native target = input * upscale factor

Submit_t g_orig_submit = nullptr;
volatile LONG g_submit_calls = 0;
bool g_installed = false;
bool g_up_ready = false;
ID3D11Texture2D *g_out[2] = {nullptr, nullptr};       // upscaled output per eye (native)
ID3D11Texture2D *g_depth[2] = {nullptr, nullptr};     // captured per-eye depth (input res)
volatile LONG g_dumped = 0;

ID3D11Device *dev_of(ID3D11Texture2D *t) { ID3D11Device *d = nullptr; if (t) t->GetDevice(&d); return d; }

// one-shot diag: confirm the submitted texture is AC's renderEye at the reduced size
uintptr_t d_(uintptr_t p, uintptr_t o) { uintptr_t a = p + o; return a < 0x10000 ? 0 : *reinterpret_cast<uintptr_t *>(a); }
void submit_diag(ID3D11Texture2D *src, int eye) {
    __try {
        uintptr_t cam = acre_get_cam();
        if (!cam) return;
        int rw = *reinterpret_cast<int *>(cam + 3944), rh = *reinterpret_cast<int *>(cam + 3948);
        uintptr_t re  = d_(cam, 1968 + (uintptr_t)eye * 176);   // viveData[eye].renderEye
        uintptr_t tex = d_(d_(re, 24), 0);                      // RenderTarget->kidColor->tex
        D3D11_TEXTURE2D_DESC sd; src->GetDesc(&sd);
        acre_log("  DIAG submit eye=%d: src=%p %ux%u | AC.renderWidth=%dx%d renderEye.tex=%p match=%d",
                 eye, (void *)src, sd.Width, sd.Height, rw, rh, (void *)tex,
                 (int)(tex == reinterpret_cast<uintptr_t>(src)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// debug: dump the upscaled eye-0 output to a raw file (gated by ACRE_DUMP)
void dump_output(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex) {
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D *stg = nullptr;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stg)) || !stg) return;
    ctx->CopyResource(stg, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) {
        FILE *f = nullptr;
        if (fopen_s(&f, "acre_upscaled.raw", "wb") == 0 && f) {   // AC working dir
            unsigned hdr[3] = {d.Width, d.Height, (unsigned)d.Format};
            fwrite(hdr, sizeof(hdr), 1, f);
            for (unsigned y = 0; y < d.Height; y++)
                fwrite((const BYTE *)m.pData + (size_t)y * m.RowPitch, 4, d.Width, f);
            fclose(f);
            acre_log("  upscale: dumped %ux%u output to acre_upscaled.raw", d.Width, d.Height);
        }
        ctx->Unmap(stg, 0);
    }
    stg->Release();
}

int STDMETHODCALLTYPE hkSubmit(void *self, int eEye, const Texture_t *tex,
                               const VRTextureBounds_t *bounds, int flags) {
    LONG n = InterlockedIncrement(&g_submit_calls);
    static volatile LONG g_diag_done = 0;
    if (tex && tex->handle && eEye >= 0 && eEye < 2 && n > 60 && InterlockedCompareExchange(&g_diag_done, 1, 0) == 0)
        submit_diag(reinterpret_cast<ID3D11Texture2D *>(tex->handle), eEye);
    if (tex && tex->handle && eEye >= 0 && eEye < 2 && g_depth[eEye]) {
        ID3D11Texture2D *src = reinterpret_cast<ID3D11Texture2D *>(tex->handle);
        ID3D11Device *dev = dev_of(src);
        if (dev) {
            ID3D11DeviceContext *ctx = nullptr; dev->GetImmediateContext(&ctx);
            if (ctx) {
                if (!g_up_ready) {
                    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
                    // target = ini override, else the native res_hook remembered, else factor
                    unsigned nw = 0, nh = 0;
                    bool res_active = acre_res_native(&nw, &nh);
                    int ow = acre_cfg_out_w(), oh = acre_cfg_out_h();
                    // if reduction is active but this frame is still full-res (stale camera),
                    // skip so the feature doesn't latch the wrong input size
                    if (res_active && !(ow > 0 && oh > 0) && d.Width >= nw) {
                        ctx->Release(); dev->Release();
                        return g_orig_submit(self, eEye, tex, bounds, flags);
                    }
                    if (ow > 0 && oh > 0) { g_out_w = ow; g_out_h = oh; }
                    else if (res_active)  { g_out_w = nw; g_out_h = nh; }
                    else {
                        float f = acre_cfg_upscale();
                        g_out_w = (unsigned)(d.Width * f + 0.5f);
                        g_out_h = (unsigned)(d.Height * f + 0.5f);
                    }
                    // never upscale DOWNward past input, that just costs frames
                    if (g_out_w < d.Width)  g_out_w = d.Width;
                    if (g_out_h < d.Height) g_out_h = d.Height;
                    g_up_ready = acre_ngx_create_upscale(ctx, d.Width, d.Height, g_out_w, g_out_h);
                }
                if (g_up_ready && !g_out[eEye]) {
                    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
                    d.Width = g_out_w; d.Height = g_out_h;
                    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
                    dev->CreateTexture2D(&d, nullptr, &g_out[eEye]);
                    acre_log("  upscale: eye=%d %ux%u -> %ux%u ready", eEye, d.Width, d.Height, g_out_w, g_out_h);
                }
                if (g_up_ready && g_out[eEye]) {
                    D3D11StateBackup bak;
                    d3d11_backup(ctx, &bak);
                    ctx->OMSetRenderTargets(0, nullptr, nullptr);
                    acre_dlss_upscale(dev, ctx, src, g_depth[eEye], eEye, g_out[eEye], acre_get_cam());
                    d3d11_restore(ctx, &bak);
                    if (eEye == 0 && n > 200 && GetEnvironmentVariableA("ACRE_DUMP", nullptr, 0) &&
                        InterlockedExchange(&g_dumped, 1) == 0)
                        dump_output(dev, ctx, g_out[eEye]);   // debug: readback to raw file
                    Texture_t myt = *tex; myt.handle = g_out[eEye];
                    ctx->Release(); dev->Release();
                    return g_orig_submit(self, eEye, &myt, bounds, flags);
                }
                ctx->Release();
            }
            dev->Release();
        }
    }
    return g_orig_submit(self, eEye, tex, bounds, flags);
}

void *patch_slot(void **vtable, int i, void *repl) {
    DWORD old;
    if (!VirtualProtect(&vtable[i], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    void *prev = vtable[i]; vtable[i] = repl;
    VirtualProtect(&vtable[i], sizeof(void *), old, &old);
    return prev;
}

}  // namespace

// Snapshot the eye's scene depth so the submit upscale can use it (called from om_hook).
extern "C" void acre_up_capture_depth(ID3D11DeviceContext *ctx, ID3D11Texture2D *depth, int eye) {
    if (!ctx || !depth || eye < 0 || eye > 1) return;
    if (!g_depth[eye]) {
        ID3D11Device *dev = dev_of(depth);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC d; depth->GetDesc(&d);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0;
        dev->CreateTexture2D(&d, nullptr, &g_depth[eye]);
        dev->Release();
    }
    if (g_depth[eye]) ctx->CopyResource(g_depth[eye], depth);
}

extern "C" bool acre_try_install_submit_hook(void) {
    if (g_installed) return true;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    void **compositor = *reinterpret_cast<void ***>(base + RVA_COMPOSITOR);
    if (!compositor) return false;
    void **vtable = *reinterpret_cast<void ***>(compositor);
    g_orig_submit = reinterpret_cast<Submit_t>(vtable[5]);
    void *prev = patch_slot(vtable, 5, reinterpret_cast<void *>(&hkSubmit));
    g_installed = prev != nullptr;
    acre_log("  submit: hook %s (compositor=%p, orig=%p)",
             g_installed ? "installed" : "FAILED", (void *)compositor, (void *)g_orig_submit);
    return g_installed;
}
