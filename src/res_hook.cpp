// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every VR render target is sized in StereoCameraVive's constructor from one call:
//   pHMD->GetRecommendedRenderTargetSize(&renderWidth, &renderHeight)   // IVRSystem vtable[0]
// Hand back a reduced size there and the whole pipeline renders small; we upscale back to
// the true native at submit. Works the same under SteamVR / OpenComposite / Oculus, and
// the user never has to touch their runtime's resolution setting.
//
// The call is one-shot mid-constructor so polling loses the race. Instead MinHook
// configureOpenVR (runs every ctor, after pHMD is set, before the size query) and patch
// the vtable from there.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include "MinHook.h"

extern "C" void  acre_log(const char *fmt, ...);
extern "C" int   acre_cfg_mode(void);
extern "C" float acre_cfg_upscale(void);
extern "C" float acre_cfg_render_scale(void);

namespace {

const uintptr_t RVA_CONFIGURE = 0x222C20;   // StereoCameraVive::configureOpenVR
const uintptr_t RVA_PHMD      = 0x155A3D8;  // vr::IVRSystem *pHMD

typedef bool (*ConfigureFn)(void *);
ConfigureFn g_orig_configure = nullptr;

typedef void(STDMETHODCALLTYPE *GetRRTSFn)(void *, uint32_t *, uint32_t *);
GetRRTSFn g_orig_getrrts = nullptr;

volatile LONG g_vtable_hooked = 0;
unsigned g_native_w = 0, g_native_h = 0;    // true recommended size (upscale target)
unsigned g_reduced_w = 0, g_reduced_h = 0;  // what CSP actually renders in dlss mode
bool     g_res_active = false;

void *patch_slot(void **vt, int i, void *repl) {
    DWORD old;
    if (!VirtualProtect(&vt[i], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    void *prev = vt[i]; vt[i] = repl;
    VirtualProtect(&vt[i], sizeof(void *), old, &old);
    return prev;
}

// remember native, hand back native/upscale in dlss mode
void STDMETHODCALLTYPE hkGetRRTS(void *self, uint32_t *pw, uint32_t *ph) {
    g_orig_getrrts(self, pw, ph);
    g_native_w = *pw; g_native_h = *ph;
    if (acre_cfg_mode() == 2 && g_native_w && g_native_h) {
        float f = acre_cfg_upscale();               // output/input ratio (2.0 perf .. 1.5 quality)
        if (f < 1.01f) f = 1.5f;
        unsigned rw = (unsigned)(g_native_w / f + 0.5f) & ~1u;   // even dims for DLSS
        unsigned rh = (unsigned)(g_native_h / f + 0.5f) & ~1u;
        if (rw < 64) rw = 64; if (rh < 64) rh = 64;
        *pw = rw; *ph = rh;
        g_reduced_w = rw; g_reduced_h = rh; g_res_active = true;
        acre_log("  res: native %ux%u -> render %ux%u (dlss upscale %.2f)",
                 g_native_w, g_native_h, rw, rh, f);
    } else if (acre_cfg_mode() == 1 && g_native_w && g_native_h &&
               acre_cfg_render_scale() > 1.005f) {
        // dlaa supersampling: render ABOVE native and let DLAA run at that size. The
        // compositor downsamples to the panel, so g_res_active stays false — there is no
        // submit-time upscale to do.
        float f = acre_cfg_render_scale();
        unsigned rw = (unsigned)(g_native_w * f + 0.5f) & ~1u;
        unsigned rh = (unsigned)(g_native_h * f + 0.5f) & ~1u;
        *pw = rw; *ph = rh;
        g_res_active = false;
        acre_log("  res: native %ux%u -> render %ux%u (dlaa render_scale %.2f, %.2fx pixels)",
                 g_native_w, g_native_h, rw, rh, f, (double)(rw * (double)rh) /
                 ((double)g_native_w * g_native_h));
    } else {
        g_res_active = false;
        acre_log("  res: native %ux%u (no reduction, mode=%d)", g_native_w, g_native_h, acre_cfg_mode());
    }
}

// runs every StereoCameraVive ctor, after pHMD is set, before the size query
bool detour_configure(void *self) {
    bool r = g_orig_configure(self);
    if (InterlockedCompareExchange(&g_vtable_hooked, 1, 0) == 0) {
        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        void *hmd = *reinterpret_cast<void **>(base + RVA_PHMD);
        if (hmd) {
            void **vt = *reinterpret_cast<void ***>(hmd);
            g_orig_getrrts = reinterpret_cast<GetRRTSFn>(vt[0]);
            patch_slot(vt, 0, reinterpret_cast<void *>(&hkGetRRTS));
            acre_log("  res: GetRecommendedRenderTargetSize hooked (pHMD=%p)", hmd);
        } else {
            InterlockedExchange(&g_vtable_hooked, 0);   // pHMD not ready — retry next ctor
            acre_log("  res: pHMD null after configureOpenVR — will retry");
        }
    }
    return r;
}

}  // namespace

// install early, before the first StereoCameraVive is built at track load
extern "C" bool acre_install_res_hook(void) {
    MH_STATUS init = MH_Initialize();            // harmless if pp_hook already initialised it
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        acre_log("  res: MH_Initialize failed (%d)", init);
        return false;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    void *target = reinterpret_cast<void *>(base + RVA_CONFIGURE);
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void *>(&detour_configure),
                                reinterpret_cast<void **>(&g_orig_configure));
    if (s == MH_OK) s = MH_EnableHook(target);
    acre_log("  res: configureOpenVR hook %s", s == MH_OK ? "ok" : "FAILED");
    return s == MH_OK;
}

// submit_hook asks this for the upscale target; false = no reduction active
extern "C" bool acre_res_native(unsigned *w, unsigned *h) {
    if (!g_res_active) return false;
    if (w) *w = g_native_w;
    if (h) *h = g_native_h;
    return true;
}
