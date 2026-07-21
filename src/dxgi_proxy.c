// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>
#include <stdio.h>
/* Exports are asm thunks (thunks.asm) that jmp through the IAT slots for
 * dxgi_real.dll. The loader fills those before any of our code runs, so there is
 * nothing to initialise here.

void acre_start_hook(void);*/
void acre_cfg_load(void);
void acre_ngx_spy_start_early(void);

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
        DisableThreadLibraryCalls(hModule);
        GetModuleFileNameA(hModule, self, MAX_PATH);
        acre_log("=== dxgi_proxy attached ===");
        acre_log("  self: %s", self);
        log_module("dwrite.dll");     /* CSP */
        log_module("dinput8.dll");
        log_module("dxgi_real.dll");
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
