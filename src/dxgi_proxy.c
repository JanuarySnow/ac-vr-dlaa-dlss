// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>
#include <stdio.h>
/* Exports are asm thunks (thunks.asm) that jmp through function pointers we fill
 * at load time from the real System32 dxgi.dll (real_ptrs.c / acre_resolve_real,
 * called from DllMain below before any export can run).

void acre_start_hook(void);*/
void acre_cfg_load(void);
void acre_ngx_spy_start_early(void);
/* real_ptrs.c: loads the pointers our export thunks jmp through. */
int acre_resolve_real(HMODULE h);

void acre_log(const char *fmt, ...) {
    char path[MAX_PATH];
    char msg[1024];
    SYSTEMTIME st;
    va_list ap;
    FILE *fh = NULL;

    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    char *slash = strrchr(path, '\\');
    if (!slash) return;
    slash[1] = '\0';
    strncat(path, "acre_proxy.log", MAX_PATH - strlen(path) - 1);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    GetLocalTime(&st);
    if (fopen_s(&fh, path, "a") == 0 && fh) {
        fprintf(fh, "%02d:%02d:%02d.%03d [%lu] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                GetCurrentProcessId(), msg);
        fclose(fh);
    }
}

static void log_module(const char *name) {
    HMODULE h = GetModuleHandleA(name);
    if (h) {
        char p[MAX_PATH];
        if (GetModuleFileNameA(h, p, MAX_PATH)) acre_log("  present: %s -> %s", name, p);
        else acre_log("  present: %s", name);
    } else {
        acre_log("  absent : %s", name);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        char self[MAX_PATH] = {0};
        wchar_t real_path[MAX_PATH];
        UINT n_sys;
        HMODULE real;
        int missing;
        DisableThreadLibraryCalls(hModule);
        GetModuleFileNameA(hModule, self, MAX_PATH);
        acre_log("=== dxgi_proxy attached ===");
        acre_log("  self: %s", self);

        /* Forward to the genuine DXGI by loading it from System32 by ABSOLUTE path.
         * Full path bypasses the loader's search order, so we get the real dxgi.dll
         * and not ourselves — no renamed dxgi_real.dll copy needed on disk. */
        n_sys = GetSystemDirectoryW(real_path, MAX_PATH);
        if (n_sys == 0 || n_sys >= MAX_PATH - 12) {
            acre_log("  FATAL: GetSystemDirectoryW failed (%u)", n_sys);
            return FALSE;
        }
        wcscat_s(real_path, MAX_PATH, L"\\dxgi.dll");
        real = LoadLibraryW(real_path);
        if (!real) {
            acre_log("  FATAL: could not load real DXGI from %ls (err %lu)",
                     real_path, GetLastError());
            return FALSE;
        }
        missing = acre_resolve_real(real);
        acre_log("  real DXGI: %ls (%d exports unresolved)", real_path, missing);
        if (missing) return FALSE;   /* a null thunk target would crash the app */

        log_module("dwrite.dll");     /* CSP */
        log_module("dinput8.dll");
        log_module("d3d11.dll");
        log_module("openvr_api.dll");
        log_module("nvngx_dlss.dll");
        acre_cfg_load();     /* read acre.ini */
        acre_ngx_spy_start_early();  /* must beat CSP's DLSS CreateFeature */
        acre_start_hook();   /* defers all D3D11 work to a thread */
        break;
    }
    case DLL_PROCESS_DETACH:
        acre_log("=== dxgi_proxy detached ===");
        break;
    default:
        break;
    }
    return TRUE;
}
