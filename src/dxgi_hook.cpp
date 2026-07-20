// SPDX-License-Identifier: GPL-3.0-or-later
// dxgi_hook — Present interception for Assetto Corsa

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdarg>

extern "C" void acre_log(const char *fmt, ...);
extern "C" bool acre_ngx_init(ID3D11Device *dev);
extern "C" bool acre_ngx_ensure_dlaa(ID3D11DeviceContext *ctx, unsigned w, unsigned h);
extern "C" void acre_cfg_poll(void);
extern "C" float acre_cfg_render_scale(void);
extern "C" void acre_cap_poll(void);
extern "C" void acre_cap_tick(void);
extern "C" int acre_cap_active(void);
extern "C" void acre_ngx_spy_install(void);
extern "C" void acre_mvhunt_install(ID3D11Device *dev);
extern "C" void acre_dlss_frame(ID3D11Device *dev, ID3D11DeviceContext *ctx, uintptr_t cam);
extern "C" void acre_install_om_hook(ID3D11DeviceContext *ctx);
extern "C" void acre_diag_preflight(void);
extern "C" void acre_diag_tick(void);
extern "C" bool acre_try_install_submit_hook(void);
extern "C" bool acre_install_res_hook(void);
extern "C" int acre_cfg_mode(void);
extern "C" int acre_cfg_ldr(void);
extern "C" int acre_cfg_ngx_spy(void);

namespace {

typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
PresentFn g_orig_present = nullptr;
volatile LONG g_present_count = 0;
volatile LONG g_dumped = 0;

// Verified offsets from DLSS_VR_NOTES.md.
const uintptr_t RVA_PYI          = 0x1559AF0;
const uintptr_t OFF_PYI_SIM      = 0x58;
const uintptr_t OFF_SIM_CAMMGR   = 392;
const uintptr_t OFF_CAMMGR_CAM   = 280;
const uintptr_t OFF_SCV_RTDEPTH  = 5096;   // rtYebisResolvedDepth
const uintptr_t OFF_SCV_RTRESOLV = 5088;   // rtYebisResolved (color)
const uintptr_t OFF_RT_KIDCOLOR  = 24;
const uintptr_t OFF_KID_TEX      = 0;

// Follow one pointer
static uintptr_t deref(uintptr_t p, uintptr_t off) {
    uintptr_t a = p + off;
    if (a < 0x10000) return 0;
    return *reinterpret_cast<uintptr_t *>(a);
}

static ID3D11Texture2D *rt_texture(uintptr_t cam, uintptr_t rt_off) {
    uintptr_t rt = deref(cam, rt_off);
    if (!rt) return nullptr;
    uintptr_t kid = deref(rt, OFF_RT_KIDCOLOR);
    if (!kid) return nullptr;
    return reinterpret_cast<ID3D11Texture2D *>(deref(kid, OFF_KID_TEX));
}

static const char *fmt_name(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_FLOAT:            return "R32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS:         return "R32_TYPELESS";
    case DXGI_FORMAT_R24G8_TYPELESS:       return "R24G8_TYPELESS";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:return "R24_UNORM_X8";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:    return "D24_UNORM_S8";
    case DXGI_FORMAT_D32_FLOAT:            return "D32_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_UNORM:       return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:   return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:   return "R32G32B32A32_FLOAT";
    default:                               return "?";
    }
}

// Copy a texture to a CPU-readable staging copy and log its format
static void dump_texture(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                         const char *label, ID3D11Texture2D *tex) {
    if (!tex) { acre_log("  dump[%s]: texture null", label); return; }

    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    acre_log("  dump[%s]: %ux%u fmt=%s(%d) samples=%u",
             label, d.Width, d.Height, fmt_name(d.Format), (int)d.Format, d.SampleDesc.Count);

    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;

    ID3D11Texture2D *staging = nullptr;
    HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        acre_log("  dump[%s]: CreateTexture2D(staging) 0x%08lx", label, (unsigned long)hr);
        return;
    }

    if (d.SampleDesc.Count > 1)
        ctx->ResolveSubresource(staging, 0, tex, 0, d.Format);
    else
        ctx->CopyResource(staging, tex);

    D3D11_MAPPED_SUBRESOURCE m;
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        acre_log("  dump[%s]: Map 0x%08lx", label, (unsigned long)hr);
        staging->Release();
        return;
    }

    // Sample five points
    const float px[5] = {0.5f, 0.25f, 0.75f, 0.5f, 0.5f};
    const float py[5] = {0.5f, 0.5f,  0.5f,  0.25f, 0.75f};
    for (int i = 0; i < 5; i++) {
        UINT x = (UINT)(px[i] * (d.Width  - 1));
        UINT y = (UINT)(py[i] * (d.Height - 1));
        const BYTE *row = (const BYTE *)m.pData + (size_t)y * m.RowPitch;
        UINT raw = *reinterpret_cast<const UINT *>(row + (size_t)x * 4);
        float f = *reinterpret_cast<const float *>(&raw);
        acre_log("    (%.2f,%.2f) px(%u,%u) raw=0x%08X  asFloat=%.6f", px[i], py[i], x, y, raw, f);
    }
    ctx->Unmap(staging, 0);
    staging->Release();
}

ID3D11Device *g_dev = nullptr;
ID3D11DeviceContext *g_ctx = nullptr;
volatile LONG g_ngx_ready = 0;

static uintptr_t live_camera() {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    uintptr_t pyi = deref(base, RVA_PYI);
    if (!pyi) return 0;                      // menu / loading
    uintptr_t sim = deref(pyi, OFF_PYI_SIM);
    uintptr_t cammgr = deref(sim, OFF_SIM_CAMMGR);
    return deref(cammgr, OFF_CAMMGR_CAM);
}

static void frame_once(IDXGISwapChain *sc) {
    uintptr_t cam = live_camera();
    // live_camera() resolves the VR StereoCameraVive, so it is null in monitor mode and
    // this early-out kept every OM hook VR-only. With ngx_spy on we still want the hooks
    // installed there, to observe how CSP's working flat-screen path renders (its motion
    // vectors in particular). Normal users are unaffected: ngx_spy defaults to 0.
    if (!cam && !acre_cfg_ngx_spy()) return;

    if (!g_dev) {                            // first live frame
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&g_dev))) || !g_dev)
            { acre_log("  frame: GetDevice failed"); return; }
        g_dev->GetImmediateContext(&g_ctx);
        if (!g_ctx) { acre_log("  frame: no context"); g_dev->Release(); g_dev = nullptr; return; }
        return;
    }

    // Re-checked every frame so ini hot-reload can do stuff
    int mode = acre_cfg_mode();               // 0 off, 1 dlaa, 2 dlss
    static bool ngx_ready = false, om_installed = false;
    if (mode != 0 && !ngx_ready) ngx_ready = acre_ngx_init(g_dev);
    // The OM hook does the scene tracking, which a mode=off reference capture needs just
    // as much as DLAA does. Gating it purely on ngx_ready meant a session launched cold
    // in mode=off never installed it, so captures armed and then silently caught nothing.
    if (!om_installed && (ngx_ready || acre_cap_active() || acre_cfg_ngx_spy())) {
        acre_install_om_hook(g_ctx);
        om_installed = true;
    }
    int ldr = acre_cfg_ldr();
    if (!cam) return;                          // rest of the pipeline needs the VR camera
    if (mode == 1 && !ldr && ngx_ready) {      // DLAA in-place on the HDR scene
        unsigned w = *reinterpret_cast<int *>(cam + 3944);
        unsigned h = *reinterpret_cast<int *>(cam + 3948);
        acre_ngx_ensure_dlaa(g_ctx, w, h);
    }
    if (mode == 2 || (mode == 1 && ldr))      // DLSS upscale, or DLAA on the LDR eye
        acre_try_install_submit_hook();
}

// handle bad pointer
static void frame_guarded(IDXGISwapChain *sc) {
    __try {
        frame_once(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        acre_log("  frame: SEH exception 0x%08lx — aborted", GetExceptionCode());
    }
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain *sc, UINT sync, UINT flags) {
    LONG n = InterlockedIncrement(&g_present_count);
    // Benchmark
    static LARGE_INTEGER freq = {}, t0 = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    if (n >= 120 && (n % 300) == 0) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (t0.QuadPart) {
            double sec = double(now.QuadPart - t0.QuadPart) / freq.QuadPart;
            acre_log("  BENCH: %.1f fps (%.2f ms/frame) over 300 frames", 300.0 / sec, sec * 1000.0 / 300.0);
        }
        t0 = now;
    }
    if (n == 1) {   // earliest point we can reach a device; track load is still ahead
        ID3D11Device *d = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&d))) && d) {
            acre_mvhunt_install(d);
            d->Release();
        }
    }
    if ((n % 30) == 0) { acre_cfg_poll(); acre_cap_poll(); }   // ini hot-reload + capture trigger
    acre_cap_tick();                        // capture pair writes/cooldown
    // Retry every frame until it takes: CSP creates its DLSS feature early, and a lazy
    // retry meant the hook landed after CreateFeature and we only ever saw Evaluate.
    acre_ngx_spy_install();
    if (n >= 120) {                         // a coupla seconds in, scene loaded
        frame_guarded(sc);
        acre_diag_tick();
    }
    return g_orig_present(sc, sync, flags);
}

// Overwrite one vtable slot, handling the page protection. Returns the old pointer.
void *patch_vtable_slot(void **vtable, int index, void *replacement) {
    DWORD old;
    if (!VirtualProtect(&vtable[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old))
        return nullptr;
    void *prev = vtable[index];
    vtable[index] = replacement;
    VirtualProtect(&vtable[index], sizeof(void *), old, &old);
    return prev;
}

bool install_present_hook() {
    // Message-only window
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "acre_dummy_wc";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "acre", WS_OVERLAPPEDWINDOW,
                                0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        acre_log("  hook: CreateWindow failed (%lu)", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 16;
    sd.BufferDesc.Height = 16;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain *sc = nullptr;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr) || !sc) {
        acre_log("  hook: D3D11CreateDeviceAndSwapChain failed (0x%08lx)", (unsigned long)hr);
        if (hwnd) DestroyWindow(hwnd);
        return false;
    }

    void **vtable = *reinterpret_cast<void ***>(sc);
    g_orig_present = reinterpret_cast<PresentFn>(vtable[8]);      // slot 8 = Present
    acre_log("  hook: dummy swapchain %p, vtable %p, orig Present %p",
             (void *)sc, (void *)vtable, (void *)g_orig_present);

    void *prev = patch_vtable_slot(vtable, 8, reinterpret_cast<void *>(&hkPresent));
    bool ok = prev != nullptr;
    acre_log("  hook: Present slot %s", ok ? "patched" : "PATCH FAILED");

    // The throwaway objects can go
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    return ok;
}

DWORD WINAPI init_thread(LPVOID) {
    // Install the render-res reduction hook first up, it must be in place
    // before AC builds StereoCameraVive . It only inline-hooks an
    // acs.exe function via MinHook
    // mode 2 always reduces res; modes 1 and 0 only when render_scale asks for
    // supersampling (mode 0 + render_scale is the ground-truth reference capture)
    if (acre_cfg_mode() == 2 || acre_cfg_render_scale() > 1.005f)
        acre_install_res_hook();
    // Let AC finish creating its own device/swapchain first avoids racing its init.
    Sleep(4000);
    acre_log("  hook: init thread starting");
    acre_diag_preflight();
    install_present_hook();
    return 0;
}

}  // namespace

// Frame clock for the om_hook's per-frame eye-counter reset
extern "C" long acre_present_count(void) { return g_present_count; }

extern "C" void acre_start_hook(void) {
    CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
}
