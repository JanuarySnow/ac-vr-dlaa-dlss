// SPDX-License-Identifier: GPL-3.0-or-later

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" void acre_log(const char *fmt, ...);
extern "C" int acre_cfg_mode(void);
extern "C" void *acre_ngx_handle_eye(int eye);

namespace {

struct State {
    int  sps          = -1;
    int  msaa         = -1;
    int  pp           = -1;
    char aa_mode[64]  = "";
    bool aa_known     = false;

    bool     hooks_installed = false;
    bool     hooks_ok        = false;
    bool     scene_seen      = false;
    unsigned scene_w = 0, scene_h = 0, scene_samples = 0, scene_array = 0;
    bool     eye_seen[2] = {false, false};

    long ticks      = 0;
    bool summarised = false;
} g;

char *dtrim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

bool ini_get(const char *path, const char *section, const char *key, char *out, size_t outsz) {
    FILE *f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return false;
    char line[512];
    bool in_sec = false, found = false;
    while (fgets(line, sizeof(line), f)) {
        char *s = dtrim(line);
        if (!*s) continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) continue;
            *e = 0;
            in_sec = (_stricmp(s + 1, section) == 0);
            continue;
        }
        if (!in_sec || *s == ';' || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        if (_stricmp(dtrim(s), key) != 0) continue;
        char *v = eq + 1;
        char *c = strchr(v, ';');
        if (c) *c = 0;
        v = dtrim(v);
        size_t n = strlen(v);
        if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') || (v[0] == '\'' && v[n - 1] == '\''))) {
            v[n - 1] = 0;
            v++;
        }
        strncpy_s(out, outsz, dtrim(v), _TRUNCATE);
        found = true;
    }
    fclose(f);
    return found;
}

bool ac_root(char *out, size_t outsz) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    char *slash = strrchr(path, '\\');
    if (!slash) return false;
    slash[1] = 0;
    strncpy_s(out, outsz, path, _TRUNCATE);
    return true;
}

bool ac_cfg_dir(char *out, size_t outsz) {
    char docs[MAX_PATH];
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs)))
        return false;
    _snprintf_s(out, outsz, _TRUNCATE, "%s\\Assetto Corsa\\cfg\\", docs);
    return true;
}

bool csp_get(const char *file, const char *section, const char *key, char *out, size_t outsz) {
    char dir[MAX_PATH], path[MAX_PATH];
    bool found = false;
    if (ac_root(dir, sizeof(dir))) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%sextension\\config\\%s", dir, file);
        if (ini_get(path, section, key, out, outsz)) found = true;
    }
    if (ac_cfg_dir(dir, sizeof(dir))) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%sextension\\%s", dir, file);
        if (ini_get(path, section, key, out, outsz)) found = true;
    }
    return found;
}

bool video_get(const char *section, const char *key, char *out, size_t outsz) {
    char dir[MAX_PATH], path[MAX_PATH];
    if (!ac_cfg_dir(dir, sizeof(dir))) return false;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%svideo.ini", dir);
    return ini_get(path, section, key, out, outsz);
}

const char *tag(int ok) { return ok < 0 ? "[??]" : ok ? "[ok]" : "[XX]"; }

}  // namespace

extern "C" void acre_diag_preflight(void) {
    char v[64];

    acre_log("=== preflight: required game / CSP settings ===");

    g.sps = csp_get("vr_tweaks.ini", "SINGLE_PASS_STEREO", "ENABLED", v, sizeof(v))
                ? (atoi(v) != 0) : -1;
    acre_log("  chk: %s CSP Single Pass Stereo = %s%s", tag(g.sps == 0),
             g.sps < 0 ? "unknown" : g.sps ? "ON" : "off",
             g.sps == 1 ? "   <- MUST BE OFF: CSP VR settings -> Single Pass Stereo. "
                          "With SPS on the per-eye scene passes we hook never appear "
                          "and DLAA stays inactive"
                        : (g.sps < 0 ? "   (vr_tweaks.ini not readable; CSP default is ON)" : ""));

    g.aa_known = csp_get("graphics_adjustments.ini", "ANTIALIASING", "MODE", v, sizeof(v));
    if (g.aa_known) strncpy_s(g.aa_mode, v, _TRUNCATE);
    int aa_off = !g.aa_known ? -1 : (_stricmp(v, "DISABLED") == 0);
    acre_log("  chk: %s CSP post-process AA = %s%s", tag(aa_off),
             g.aa_known ? g.aa_mode : "unknown",
             aa_off == 0 ? "   <- SHOULD BE DISABLED: CSP Graphics Adjustments -> "
                           "Antialiasing -> Disabled. It runs after DLAA and re-softens the image"
                         : (aa_off < 0 ? "   (graphics_adjustments.ini not readable)" : ""));

    g.pp = video_get("POST_PROCESS", "ENABLED", v, sizeof(v)) ? (atoi(v) != 0) : -1;
    acre_log("  chk: %s post-processing = %s%s", tag(g.pp == 1),
             g.pp < 0 ? "unknown" : g.pp ? "on" : "OFF",
             g.pp == 0 ? "   <- MUST BE ON: AC video settings -> Post-processing. "
                         "DLAA runs on the HDR scene; with it off there is no HDR target to hook"
                       : (g.pp < 0 ? "   (video.ini not readable)" : ""));

    g.msaa = video_get("VIDEO", "AASAMPLES", v, sizeof(v)) ? atoi(v) : -1;
    acre_log("  chk: %s MSAA (AASAMPLES) = %s%s", tag(g.msaa == 1),
             g.msaa < 0 ? "unknown" : g.msaa == 1 ? "off" : v,
             g.msaa > 1 ? "   <- MUST BE OFF: AC / Content Manager video settings -> "
                          "Anti-aliasing -> off (AASAMPLES=1). DLSS replaces MSAA and "
                          "cannot consume a multisampled scene target"
                        : (g.msaa < 0 ? "   (video.ini not readable)" : ""));
}

extern "C" void acre_diag_hooks(int ok) {
    g.hooks_installed = true;
    g.hooks_ok = (ok != 0);
    acre_log("  chk: %s render hooks installed", tag(g.hooks_ok));
}

extern "C" void acre_diag_scene(unsigned w, unsigned h, unsigned samples, unsigned arraysize) {
    if (g.scene_seen) return;
    g.scene_seen = true;
    g.scene_w = w;
    g.scene_h = h;
    g.scene_samples = samples;
    g.scene_array = arraysize;
    acre_log("  chk: %s scene target found %ux%u samples=%u arraysize=%u",
             tag(samples == 1 && arraysize == 1), w, h, samples, arraysize);
    if (samples > 1 && acre_cfg_mode() == 0)
        acre_log("  chk: [ok] scene target is %ux MSAA and mode=off — reference capture, "
                 "not a misconfiguration", samples);
    else if (samples > 1)
        acre_log("  chk: [XX] scene target is %ux MSAA — DLAA cannot run. "
                 "Turn Anti-aliasing off in AC / Content Manager video settings", samples);
    if (arraysize > 1)
        acre_log("  chk: [XX] scene target is a %u-slice array — this is Single Pass Stereo. "
                 "Turn it off in CSP VR settings", arraysize);
}

extern "C" void acre_diag_eye(int eye) {
    if (eye < 0 || eye > 1 || g.eye_seen[eye]) return;
    g.eye_seen[eye] = true;
    acre_log("  chk: [ok] DLAA dispatched on eye %d", eye);
}

// One-shot: dump the matrices CSP actually renders with (its transform CB) next to the
// AC-side matrices acre_dlss_inplace builds motion vectors from. If these disagree, the
// MVs are unprojecting CSP's depth buffer through the wrong projection — which shows up
// as distance shimmer and softness in motion, worst where depth error is largest.
extern "C" void acre_diag_matrices(const float *cb, uintptr_t cam) {
    static bool done = false;
    if (done || !cb || !cam) return;
    done = true;

    const float *csp_view = cb + 0;
    const float *csp_proj = cb + 16;

    acre_log("=== matrix cross-check (CSP render CB vs AC struct) ===");
    for (int r = 0; r < 4; r++)
        acre_log("  mat: CSP view  [%d] %10.4f %10.4f %10.4f %10.4f", r,
                 csp_view[r * 4 + 0], csp_view[r * 4 + 1], csp_view[r * 4 + 2], csp_view[r * 4 + 3]);
    for (int r = 0; r < 4; r++)
        acre_log("  mat: CSP proj  [%d] %10.4f %10.4f %10.4f %10.4f", r,
                 csp_proj[r * 4 + 0], csp_proj[r * 4 + 1], csp_proj[r * 4 + 2], csp_proj[r * 4 + 3]);
    acre_log("  mat: CSP camPos (%.3f %.3f %.3f)  near=%.4f far=%.1f fov=%.2f  render=%.0fx%.0f",
             cb[48], cb[49], cb[50], cb[52], cb[53], cb[54], cb[56], cb[57]);

    __try {
        const float *ac_view = reinterpret_cast<const float *>(cam + 2512);
        for (int r = 0; r < 4; r++)
            acre_log("  mat: AC view   [%d] %10.4f %10.4f %10.4f %10.4f", r,
                     ac_view[r * 4 + 0], ac_view[r * 4 + 1], ac_view[r * 4 + 2], ac_view[r * 4 + 3]);
        for (int eye = 0; eye < 2; eye++) {
            const float *ac_proj =
                reinterpret_cast<const float *>(cam + 1968 + eye * 176 + 72);
            for (int r = 0; r < 4; r++)
                acre_log("  mat: AC eyeProj[%d][%d] %10.4f %10.4f %10.4f %10.4f", eye, r,
                         ac_proj[r * 4 + 0], ac_proj[r * 4 + 1], ac_proj[r * 4 + 2],
                         ac_proj[r * 4 + 3]);
        }

        float vdiff = 0;
        for (int i = 0; i < 16; i++) {
            float d = csp_view[i] - ac_view[i];
            if (d < 0) d = -d;
            if (d > vdiff) vdiff = d;
        }
        acre_log("  mat: max |CSP view - AC view| = %.6f  -> view matrices %s", vdiff,
                 vdiff < 0.001f ? "AGREE" : "DISAGREE (MVs use the wrong view)");
        acre_log("  mat: CSP proj M11=%.4f M22=%.4f (1.0 means FOV applied OUTSIDE the "
                 "matrix, so AC eyeProj is NOT the projection the depth buffer was "
                 "rendered with)", csp_proj[0], csp_proj[5]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        acre_log("  mat: SEH reading AC camera matrices");
    }
}

// Distance between the view translations the two eyes' DLAA dispatches used this frame.
// Should always be ~IPD (0.05-0.09 m). Near-zero = the eye/view pairing raced and one
// eye's MVs were built from the other eye's camera.
extern "C" void acre_diag_eyedist(float d) {
    static long n = 0, races = 0, ipdlike = 0;
    static float mn = 1e9f, mx = 0;
    n++;
    if (d < mn) mn = d;
    if (d > mx) mx = d;
    if (d < 0.02f) {
        races++;
        if (races <= 5)
            acre_log("  chk: [XX] eye0/eye1 view distance %.4f m — eye/view pairing RACE "
                     "(both eyes used the same camera)", d);
    } else if (d > 0.05f && d < 0.09f) {
        ipdlike++;
    }
    if ((n % 300) == 0)
        acre_log("  chk: eye view separation, %ld frames: min=%.4f max=%.4f m "
                 "ipd-like=%ld near-zero=%ld other=%ld",
                 n, mn, mx, ipdlike, races, n - ipdlike - races);
}

// Per-frame count of jittered-CB substitutions per eye. One eye jittered while the other
// is not = mismatched edge aliasing between eyes = jaggies fused at a phantom depth.
extern "C" void acre_diag_jitter_subs(int e0, int e1) {
    static long n = 0, asym = 0, none = 0;
    n++;
    if ((e0 == 0) != (e1 == 0)) {
        asym++;
        if (asym <= 5)
            acre_log("  chk: [XX] jitter CB substituted for %s only this frame (e0=%d e1=%d) "
                     "— per-eye jitter asymmetry", e0 ? "eye 0" : "eye 1", e0, e1);
    } else if (e0 == 0 && e1 == 0) {
        none++;
    }
    if ((n % 300) == 0)
        acre_log("  chk: jitter substitution, %ld frames: both-eyes=%ld one-eye=%ld none=%ld",
                 n, n - asym - none, asym, none);
}

extern "C" void acre_diag_tick(void) {
    if (!g.hooks_installed || g.summarised) return;
    if (++g.ticks < 600) return;
    g.summarised = true;

    bool both = g.eye_seen[0] && g.eye_seen[1];
    bool feats = acre_ngx_handle_eye(0) != nullptr && acre_ngx_handle_eye(1) != nullptr;

    acre_log("=== summary ===");
    acre_log("  chk: %s hooks %s", tag(g.hooks_ok), g.hooks_ok ? "installed" : "FAILED TO INSTALL");
    acre_log("  chk: %s DLSS features created: eye0=%s eye1=%s", tag(feats),
             acre_ngx_handle_eye(0) ? "yes" : "no", acre_ngx_handle_eye(1) ? "yes" : "no");
    acre_log("  chk: %s DLAA running on both eyes (eye0=%s eye1=%s)", tag(both),
             g.eye_seen[0] ? "yes" : "no", g.eye_seen[1] ? "yes" : "no");

    if (!g.scene_seen) {
        acre_log("  chk: [XX] no per-eye HDR scene target was ever seen — DLAA is NOT active");
        if (g.pp == 0)
            acre_log("       most likely cause: post-processing is off in AC video settings");
        else if (g.sps == 1)
            acre_log("       most likely cause: Single Pass Stereo is on in CSP VR settings");
        else
            acre_log("       check post-processing is on and Single Pass Stereo is off");
    } else if (!both) {
        acre_log("  chk: [XX] DLAA reached %s eye only — expected both",
                 g.eye_seen[0] ? "the left" : g.eye_seen[1] ? "the right" : "neither");
        if (g.sps == 1 || g.scene_array > 1)
            acre_log("       most likely cause: Single Pass Stereo is on in CSP VR settings");
    } else if (acre_cfg_mode() == 1) {
        acre_log("  chk: [ok] DLAA active on both eyes");
    }
}
