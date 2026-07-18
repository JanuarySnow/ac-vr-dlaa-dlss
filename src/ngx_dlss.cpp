// SPDX-License-Identifier: GPL-3.0-or-later

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

extern "C" void acre_log(const char *fmt, ...);
extern "C" int acre_cfg_preset(void);
extern "C" int acre_cfg_perfquality(void);
extern "C" int acre_cfg_auto_exposure(void);

namespace {
bool g_ngx_inited = false;
NVSDK_NGX_Parameter *g_params = nullptr;

// Init_with_ProjectID wants a GUID-format project id; a non-GUID string returns
// FAIL_InvalidParameter (0xBAD00005). CUSTOM engine needs no registered app id.
const char *kProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";

// NGX writes logs into the app-data path and rejects a relative one, so hand it the
// absolute AC folder (from the main module).
const wchar_t *app_data_path() {
    static wchar_t dir[MAX_PATH];
    if (dir[0]) return dir;
    DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { wcscpy_s(dir, L"."); return dir; }
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = L'\0';
    return dir;
}
}  // namespace

extern "C" bool acre_ngx_init(ID3D11Device *dev) {
    if (g_ngx_inited) return true;
    if (!dev) { acre_log("  ngx: null device"); return false; }

    const wchar_t *path = app_data_path();
    acre_log("  ngx: app data path = %ls", path);
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1",
        path, dev, nullptr, NVSDK_NGX_Version_API);
    acre_log("  ngx: Init_with_ProjectID -> 0x%08X (%ls)",
             (unsigned)r, GetNGXResultAsString(r));
    if (NVSDK_NGX_FAILED(r)) return false;

    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(r) || !g_params) {
        acre_log("  ngx: GetCapabilityParameters -> 0x%08X", (unsigned)r);
        return false;
    }

    int dlss_available = 0;
    NVSDK_NGX_Result qr =
        g_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlss_available);
    int needs_driver = 0;
    g_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    int min_driver_major = 0, min_driver_minor = 0;
    g_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_driver_major);
    g_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_driver_minor);

    acre_log("  ngx: DLSS available=%d (q=0x%08X) needsDriverUpdate=%d minDriver=%d.%d",
             dlss_available, (unsigned)qr, needs_driver, min_driver_major, min_driver_minor);

    g_ngx_inited = (dlss_available != 0);
    return g_ngx_inited;
}

extern "C" NVSDK_NGX_Parameter *acre_ngx_params(void) { return g_params; }

namespace {
NVSDK_NGX_Handle *g_dlss = nullptr;
NVSDK_NGX_Parameter *g_dlss_params = nullptr;
}  // namespace

// One feature PER EYE: DLSS keeps temporal history per feature, and sharing one across
// eyes makes distant geometry oscillate with the stereo offset.
static NVSDK_NGX_Handle *g_dlss2[2] = {nullptr, nullptr};
static NVSDK_NGX_Parameter *g_dlss_params2[2] = {nullptr, nullptr};

// called every frame in dlaa mode; recreates the features when preset/auto_exposure
// changes in the ini
extern "C" bool acre_ngx_create_dlaa(ID3D11DeviceContext *ctx, unsigned w, unsigned h);
extern "C" bool acre_ngx_ensure_dlaa(ID3D11DeviceContext *ctx, unsigned w, unsigned h) {
    static int cur_preset = -1, cur_ae = -1;
    int p = acre_cfg_preset(), ae = acre_cfg_auto_exposure();
    if (g_dlss2[0] && (cur_preset != -1) && (p != cur_preset || ae != cur_ae)) {
        for (int eye = 0; eye < 2; eye++) {
            if (g_dlss2[eye]) { NVSDK_NGX_D3D11_ReleaseFeature(g_dlss2[eye]); g_dlss2[eye] = nullptr; }
            if (g_dlss_params2[eye]) { NVSDK_NGX_D3D11_DestroyParameters(g_dlss_params2[eye]); g_dlss_params2[eye] = nullptr; }
        }
        acre_log("  ngx: DLAA features released (preset/auto_exposure changed -> recreate)");
    }
    bool ok = acre_ngx_create_dlaa(ctx, w, h);
    if (ok) { cur_preset = p; cur_ae = ae; }
    return ok;
}

extern "C" bool acre_ngx_create_dlaa(ID3D11DeviceContext *ctx, unsigned w, unsigned h) {
    if (g_dlss2[0] && g_dlss2[1]) return true;
    if (!g_ngx_inited || !ctx) { acre_log("  ngx: create skipped (init=%d)", g_ngx_inited); return false; }

    bool ok = true;
    for (int eye = 0; eye < 2; eye++) {
        NVSDK_NGX_Result r = NVSDK_NGX_D3D11_AllocateParameters(&g_dlss_params2[eye]);
        if (NVSDK_NGX_FAILED(r) || !g_dlss_params2[eye]) {
            acre_log("  ngx: AllocateParameters[%d] -> 0x%08X", eye, (unsigned)r); ok = false; break;
        }
        // Render preset from config (K = transformer, best on thin distant edges).
        g_dlss_params2[eye]->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                 (unsigned)acre_cfg_preset());
        NVSDK_NGX_DLSS_Create_Params cp = {};
        cp.Feature.InWidth = w; cp.Feature.InHeight = h;
        cp.Feature.InTargetWidth = w; cp.Feature.InTargetHeight = h;   // DLAA
        cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
        // no AutoExposure by default, its estimate flickers on dark high-contrast edges
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                  NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        if (acre_cfg_auto_exposure())
            cp.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_dlss2[eye], g_dlss_params2[eye], &cp);
        acre_log("  ngx: CreateDLSS[eye %d] (DLAA %ux%u) -> 0x%08X (%ls) handle=%p",
                 eye, w, h, (unsigned)r, GetNGXResultAsString(r), (void *)g_dlss2[eye]);
        if (NVSDK_NGX_FAILED(r) || !g_dlss2[eye]) { ok = false; break; }
    }
    return ok;
}

extern "C" void *acre_ngx_handle_eye(int eye) { return (eye >= 0 && eye < 2) ? g_dlss2[eye] : nullptr; }
extern "C" void *acre_ngx_eval_params_eye(int eye) { return (eye >= 0 && eye < 2) ? g_dlss_params2[eye] : nullptr; }

// upscale features (input res -> output res), one per eye, LDR input
static NVSDK_NGX_Handle *g_up[2] = {nullptr, nullptr};
static NVSDK_NGX_Parameter *g_up_params[2] = {nullptr, nullptr};

extern "C" bool acre_ngx_create_upscale(ID3D11DeviceContext *ctx, unsigned inW, unsigned inH,
                                        unsigned outW, unsigned outH) {
    if (g_up[0] && g_up[1]) return true;
    if (!g_ngx_inited || !ctx) return false;
    for (int eye = 0; eye < 2; eye++) {
        if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&g_up_params[eye]))) return false;
        g_up_params[eye]->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                              (unsigned)acre_cfg_preset());
        NVSDK_NGX_DLSS_Create_Params cp = {};
        cp.Feature.InWidth = inW; cp.Feature.InHeight = inH;
        cp.Feature.InTargetWidth = outW; cp.Feature.InTargetHeight = outH;
        cp.Feature.InPerfQualityValue = (NVSDK_NGX_PerfQuality_Value)acre_cfg_perfquality();
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_up[eye], g_up_params[eye], &cp);
        acre_log("  ngx: CreateUpscale[eye %d] %ux%u->%ux%u -> 0x%08X (%ls)",
                 eye, inW, inH, outW, outH, (unsigned)r, GetNGXResultAsString(r));
        if (NVSDK_NGX_FAILED(r) || !g_up[eye]) return false;
    }
    return true;
}
extern "C" void *acre_ngx_up_handle(int eye) { return (eye >= 0 && eye < 2) ? g_up[eye] : nullptr; }
extern "C" void *acre_ngx_up_params(int eye) { return (eye >= 0 && eye < 2) ? g_up_params[eye] : nullptr; }
// Back-compat single-feature accessors (used by the unused Present-hook path).
extern "C" void *acre_ngx_handle(void) { return g_dlss2[0]; }
extern "C" void *acre_ngx_eval_params(void) { return g_dlss_params2[0]; }
