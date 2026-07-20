// SPDX-License-Identifier: GPL-3.0-or-later
// config read acre.ini from the AC folder at startup so DLAA / DLSS mode, preset, and
// tuning can be switched without rebuilding. Exposed as extern "C" accessors used across
// the proxy. Loaded once in DllMain

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" void acre_log(const char *fmt, ...);

namespace {

struct Cfg {
    int  mode = 1;              // 0 off, 1 dlaa, 2 dlss (upscale)
    int  preset = 11;          // DLSS render preset (K=11)
    float upscale = 1.5f;      // dlss output/input ratio
    float render_scale = 1.0f; // dlaa supersample: render target = native * this
    int  out_w = 0, out_h = 0; // absolute per-eye output target
    int  perfquality = 1;      // NVSDK_NGX_PerfQuality_Value (MaxQuality=1) for dlss
    int  jitter = 1;
    int  jflip_x = 0, jflip_y = 0;   // sign of InJitterOffset reported to DLSS (calibration)
    int  mv_flip = 0;                // negate motion vectors via InMVScale (calibration)
    int  reactive = 1;
    float mask_scale = 3.0f;
    int  auto_exposure = 0;
    float sharpness = 0.0f;
    bool loaded = false;
    FILETIME mtime = {};        // last seen acre.ini write time
} g;

int preset_from_letter(char c) {
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'G') return 1 + (c - 'A');   // A..G = 1..7
    switch (c) { case 'J': return 10; case 'K': return 11; case 'L': return 12; case 'M': return 13; }
    return 11;   // default K
}

void set_upscale(const char *v) {
    if (!_stricmp(v, "performance"))    { g.upscale = 2.0f;  g.perfquality = 0; }   // MaxPerf
    else if (!_stricmp(v, "balanced"))  { g.upscale = 1.72f; g.perfquality = 2; }   // Balanced
    else                                { g.upscale = 1.5f;  g.perfquality = 1; }   // Quality
}

// Supersampling is quadratic in cost, so refuse silly values: 2.0 is already 4x the pixels.
// Below 1.0 would mean rendering under native, which is what mode=dlss is for.
float clamp_render_scale(float f) {
    if (!(f > 0.0f)) return 1.0f;
    if (f < 1.0f) return 1.0f;
    if (f > 2.0f) return 2.0f;
    return f;
}

char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

bool ini_path(char *path) {
    DWORD n = GetModuleFileNameA(GetModuleHandleA(nullptr), path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    char *slash = strrchr(path, '\\');
    if (!slash) return false;
    slash[1] = 0;
    strncat(path, "acre.ini", MAX_PATH - strlen(path) - 1);
    return true;
}

void load() {
    g.loaded = true;
    char path[MAX_PATH];
    if (!ini_path(path)) return;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        g.mtime = fad.ftLastWriteTime;

    FILE *f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) { acre_log("  cfg: no acre.ini, using defaults"); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (!_stricmp(key, "mode"))
            g.mode = !_stricmp(val, "off") ? 0 : !_stricmp(val, "dlss") ? 2 : 1;
        else if (!_stricmp(key, "preset"))        g.preset = preset_from_letter(val[0]);
        else if (!_stricmp(key, "upscale"))       set_upscale(val);
        else if (!_stricmp(key, "render_scale"))  g.render_scale = clamp_render_scale((float)atof(val));
        else if (!_stricmp(key, "output_width"))  g.out_w = atoi(val);
        else if (!_stricmp(key, "output_height")) g.out_h = atoi(val);
        else if (!_stricmp(key, "jitter"))        g.jitter = atoi(val);
        else if (!_stricmp(key, "jitter_flip_x")) g.jflip_x = atoi(val);
        else if (!_stricmp(key, "jitter_flip_y")) g.jflip_y = atoi(val);
        else if (!_stricmp(key, "mv_flip"))       g.mv_flip = atoi(val);
        else if (!_stricmp(key, "reactive_mask")) g.reactive = atoi(val);
        else if (!_stricmp(key, "mask_scale"))    g.mask_scale = (float)atof(val);
        else if (!_stricmp(key, "auto_exposure")) g.auto_exposure = atoi(val);
        else if (!_stricmp(key, "sharpness"))     g.sharpness = (float)atof(val);
    }
    fclose(f);
    acre_log("  cfg: mode=%d preset=%d upscale=%.2f render_scale=%.2f out=%dx%d pq=%d jitter=%d "
             "reactive=%d autoexp=%d sharp=%.2f",
             g.mode, g.preset, g.upscale, g.render_scale, g.out_w, g.out_h, g.perfquality,
             g.jitter, g.reactive, g.auto_exposure, g.sharpness);
}

Cfg &cfg() { if (!g.loaded) load(); return g; }

}  // namespace

extern "C" void acre_cfg_load(void) { cfg(); }

// re-parse acre.ini when its write time changes
// every 30 frame or so (a file-attribute stat, negligible). Live-appliable keys take effect
// within a second of saving
extern "C" void acre_cfg_poll(void) {
    if (!g.loaded) { load(); return; }
    char path[MAX_PATH];
    if (!ini_path(path)) return;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return;
    if (CompareFileTime(&fad.ftLastWriteTime, &g.mtime) == 0) return;
    int old_dlss = (g.mode == 2);
    float old_up = g.upscale, old_rs = g.render_scale;
    load();                                     // re-parse + log the new cfg line
    if ((g.mode == 2) != old_dlss || g.upscale != old_up || g.render_scale != old_rs)
        acre_log("  cfg: NOTE mode dlaa<->dlss, upscale ratio and render_scale need a SESSION "
                 "RESTART (render targets are sized at track load)");
}
extern "C" int   acre_cfg_mode(void)        { return cfg().mode; }
extern "C" int   acre_cfg_preset(void)      { return cfg().preset; }
extern "C" float acre_cfg_upscale(void)     { return cfg().upscale; }
extern "C" float acre_cfg_render_scale(void) { return cfg().render_scale; }
extern "C" int   acre_cfg_out_w(void)       { return cfg().out_w; }
extern "C" int   acre_cfg_out_h(void)       { return cfg().out_h; }
extern "C" int   acre_cfg_perfquality(void) { return cfg().perfquality; }
extern "C" int   acre_cfg_jitter(void)      { return cfg().jitter; }
extern "C" int   acre_cfg_jflip_x(void)     { return cfg().jflip_x; }
extern "C" int   acre_cfg_jflip_y(void)     { return cfg().jflip_y; }
extern "C" int   acre_cfg_mv_flip(void)     { return cfg().mv_flip; }
extern "C" int   acre_cfg_reactive(void)    { return cfg().reactive; }
extern "C" float acre_cfg_mask_scale(void)  { return cfg().mask_scale; }
extern "C" int   acre_cfg_auto_exposure(void) { return cfg().auto_exposure; }
extern "C" float acre_cfg_sharpness(void)   { return cfg().sharpness; }
