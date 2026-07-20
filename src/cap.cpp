// SPDX-License-Identifier: GPL-3.0-or-later
// cap - offline-analysis frame capture, in adjacent-frame PAIRS.
//
// A synchronous per-frame readback stalls the pipeline so hard that consecutive
// "frames" end up ~1s of game time apart — DLSS history breaks and the metrics measure
// the capture, not the game. Instead: frames A and B of a pair are only CopyResource'd
// into pre-allocated staging (cheap, async), the map+disk-write happens afterwards
// during a cooldown, then the next pair starts. Temporal metrics use the clean A->B
// deltas; the stall lands between pairs where it pollutes nothing.
//
// Trigger: acre_capture.txt in the AC folder containing the number of PAIRS.
// Output:  acre_captures\<stamp>\p{pair}_f{0|1}_e{eye}_{in,out,mv}.raw

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>

extern "C" void acre_log(const char *fmt, ...);
extern "C" void acre_cap_mark(int eye);

namespace {

enum Stream { S_IN = 0, S_OUT = 1, S_MV = 2 };

struct Slot {
    ID3D11Texture2D *stg = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    bool filled = false;
};

char g_dir[MAX_PATH];
int  g_pairs_wanted = 0, g_pairs_done = 0;
int  g_state = 0;              // 0 idle, 1 frame A, 2 frame B, 3 write, 4 cooldown
int  g_cooldown = 0;
Slot g_slot[2][2][3];          // [frame A/B][eye][stream]
ID3D11Device *g_cap_dev = nullptr;
ID3D11DeviceContext *g_cap_ctx = nullptr;

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

unsigned fmt_bpp(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return 4;
    default:                             return 0;
    }
}

// async half: GPU copy into the pooled staging slot, no map
void grab(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex,
          int frame, int eye, int stream) {
    if (!tex || frame < 0 || frame > 1) return;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    if (!fmt_bpp(d.Format)) return;
    Slot &s = g_slot[frame][eye][stream];
    if (s.stg && (s.desc.Width != d.Width || s.desc.Height != d.Height ||
                  s.desc.Format != d.Format)) {
        s.stg->Release();
        s.stg = nullptr;
    }
    if (!s.stg) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &s.stg)) || !s.stg) return;
        s.desc = sd;
    }
    ctx->CopyResource(s.stg, tex);
    s.filled = true;
    g_cap_dev = dev;
    g_cap_ctx = ctx;
}

// slow half: map + write, only ever runs in the write state between pairs
bool write_slot(Slot &s, const char *path) {
    if (!s.filled || !s.stg || !g_cap_ctx) return false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_cap_ctx->Map(s.stg, 0, D3D11_MAP_READ, 0, &m))) return false;
    unsigned bpp = fmt_bpp(s.desc.Format);
    FILE *f = nullptr;
    bool ok = false;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        unsigned hdr[4] = {s.desc.Width, s.desc.Height, (unsigned)s.desc.Format, bpp};
        fwrite(hdr, sizeof(hdr), 1, f);
        for (unsigned y = 0; y < s.desc.Height; y++)
            fwrite((const BYTE *)m.pData + (size_t)y * m.RowPitch,
                   (size_t)s.desc.Width * bpp, 1, f);
        fclose(f);
        ok = true;
    }
    g_cap_ctx->Unmap(s.stg, 0);
    return ok;
}

const char *stream_name(int s) { return s == S_IN ? "in" : s == S_OUT ? "out" : "mv"; }

}  // namespace

extern "C" void acre_cap_poll(void) {
    if (g_state != 0) return;
    char root[MAX_PATH], trig[MAX_PATH];
    if (!ac_root(root, sizeof(root))) return;
    _snprintf_s(trig, sizeof(trig), _TRUNCATE, "%sacre_capture.txt", root);
    FILE *f = nullptr;
    if (fopen_s(&f, trig, "r") != 0 || !f) return;
    int n = 0;
    fscanf_s(f, "%d", &n);
    fclose(f);
    remove(trig);
    if (n <= 0) n = 4;
    if (n > 16) n = 16;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char base[MAX_PATH];
    _snprintf_s(base, sizeof(base), _TRUNCATE, "%sacre_captures", root);
    CreateDirectoryA(base, nullptr);
    _snprintf_s(g_dir, sizeof(g_dir), _TRUNCATE, "%s\\%02d%02d_%02d%02d%02d",
                base, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryA(g_dir, nullptr);
    g_pairs_wanted = n;
    g_pairs_done = 0;
    g_state = 1;
    acre_log("  cap: capturing %d frame pairs -> %s", n, g_dir);
}

extern "C" int acre_cap_active(void) { return g_state == 1 || g_state == 2; }

extern "C" void acre_cap_in(ID3D11Device *dev, ID3D11DeviceContext *ctx, int eye,
                            ID3D11Texture2D *color) {
    if (g_state == 1 || g_state == 2) grab(dev, ctx, color, g_state - 1, eye, S_IN);
}

extern "C" void acre_cap_out(ID3D11Device *dev, ID3D11DeviceContext *ctx, int eye,
                             ID3D11Texture2D *out, ID3D11Texture2D *mv) {
    if (g_state != 1 && g_state != 2) return;
    grab(dev, ctx, out, g_state - 1, eye, S_OUT);
    grab(dev, ctx, mv, g_state - 1, eye, S_MV);
    acre_cap_mark(eye);
}

// frame boundary; also the only place states advance during capture
extern "C" void acre_cap_mark(int eye) {
    if (eye != 1) return;
    if (g_state == 1) g_state = 2;
    else if (g_state == 2) g_state = 3;
}

// called once per Present: does the disk writes between pairs
extern "C" void acre_cap_tick(void) {
    if (g_state == 3) {
        char p[MAX_PATH];
        for (int fr = 0; fr < 2; fr++)
            for (int eye = 0; eye < 2; eye++)
                for (int s = 0; s < 3; s++) {
                    Slot &sl = g_slot[fr][eye][s];
                    if (!sl.filled) continue;
                    _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\p%02d_f%d_e%d_%s.raw",
                                g_dir, g_pairs_done, fr, eye, stream_name(s));
                    write_slot(sl, p);
                    sl.filled = false;
                }
        g_pairs_done++;
        if (g_pairs_done >= g_pairs_wanted) {
            g_state = 0;
            acre_log("  cap: done, %d pairs in %s", g_pairs_done, g_dir);
        } else {
            g_state = 4;
            g_cooldown = 30;
        }
    } else if (g_state == 4) {
        if (--g_cooldown <= 0) g_state = 1;
    } else if (g_state == 1 || g_state == 2) {
        // no dispatches feeding us — abort instead of wedging the state machine
        static int stuck = 0;
        if (++stuck > 900) {
            acre_log("  cap: no frames arrived in 900 ticks — capture aborted");
            g_state = 0;
            stuck = 0;
        }
    }
}
