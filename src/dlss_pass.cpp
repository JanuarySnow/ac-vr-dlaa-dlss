// SPDX-License-Identifier: GPL-3.0-or-later
// dlss_pass per-frame reprojection + DLSS evaluate
//
// Runs the reproject compute shader  then DLSS DLAA on ACs
// resolved HDR color, writing to a  output. verifies the evaluate produces
// 
//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <cstring>
#include <cmath>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "reproject_cs.h"
#include "reactive_mask_cs.h"
#include "sharpen_cs.h"

extern "C" void acre_log(const char *fmt, ...);
extern "C" void *acre_ngx_handle(void);
extern "C" void *acre_ngx_eval_params(void);
extern "C" void *acre_ngx_handle_eye(int eye);
extern "C" void *acre_ngx_eval_params_eye(int eye);
extern "C" void acre_get_jitter(float *jx, float *jy);   // om_hook.cpp
extern "C" void *acre_ngx_up_handle(int eye);
extern "C" void *acre_ngx_up_params(int eye);
extern "C" int acre_cfg_reactive(void);
extern "C" float acre_cfg_mask_scale(void);
extern "C" int acre_cfg_jflip_x(void);
extern "C" int acre_cfg_jflip_y(void);
extern "C" int acre_cfg_mv_flip(void);

// MV sign as handed to DLSS
static float mv_scale(void) { return acre_cfg_mv_flip() ? -1.0f : 1.0f; }

extern "C" float acre_cfg_sharpness(void);

// sharpenng
namespace {
ID3D11ComputeShader *g_sh_cs = nullptr;
ID3D11Buffer *g_sh_cb = nullptr;
struct ShCB { float dim[2]; float sharp; float hdr; };

bool sharpen_ready(ID3D11Device *dev) {
    if (g_sh_cs && g_sh_cb) return true;
    if (!g_sh_cs &&
        FAILED(dev->CreateComputeShader(g_sharpen_cs, sizeof(g_sharpen_cs), nullptr, &g_sh_cs)))
        return false;
    if (!g_sh_cb) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(ShCB); bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_sh_cb))) return false;
    }
    return true;
}

// src -> dst (same size). hdr=1 for pre-tonemap RGBA16F input.
void sharpen_pass(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                  ID3D11ShaderResourceView *src, ID3D11UnorderedAccessView *dst,
                  unsigned w, unsigned h, float amount, int hdr) {
    if (!sharpen_ready(dev)) return;
    ShCB cb = {(float)w, (float)h, amount, (float)hdr};
    ctx->UpdateSubresource(g_sh_cb, 0, nullptr, &cb, 0, 0);
    ctx->CSSetShader(g_sh_cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_sh_cb);
    ctx->CSSetShaderResources(0, 1, &src);
    ctx->CSSetUnorderedAccessViews(0, 1, &dst, nullptr);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11UnorderedAccessView *nu = nullptr; ID3D11ShaderResourceView *ns = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    ctx->CSSetShaderResources(0, 1, &ns);
}
}  // namespace

// Jitter as reported to DLSS
static void reported_jitter(float *jx, float *jy) {
    acre_get_jitter(jx, jy);
    if (acre_cfg_jflip_x()) *jx = -*jx;
    if (acre_cfg_jflip_y()) *jy = -*jy;
}

namespace {

const uintptr_t OFF_SCV_PROJ     = 2448;
const uintptr_t OFF_SCV_VIEW     = 2512;
const uintptr_t OFF_SCV_RTDEPTH  = 5096;
const uintptr_t OFF_SCV_RTRESOLV = 5088;
const uintptr_t OFF_RT_KIDCOLOR  = 24;
const uintptr_t OFF_KID_TEX      = 0;

ID3D11ComputeShader *g_cs = nullptr;
ID3D11Texture2D *g_mv = nullptr;       ID3D11UnorderedAccessView *g_mv_uav = nullptr;
ID3D11Texture2D *g_out = nullptr;      ID3D11UnorderedAccessView *g_out_uav = nullptr;
ID3D11ShaderResourceView *g_depth_srv = nullptr;
ID3D11Buffer *g_cb = nullptr;
ID3D11Texture2D *g_readback = nullptr;
unsigned g_w = 0, g_h = 0;
bool g_ready = false;
bool g_have_prev = false;
float g_vp_prev[16];
long g_frame = 0;

struct CB { float reproj[16]; float dim[2]; float pad[2]; };   // clip->clip + dims

uintptr_t deref(uintptr_t p, uintptr_t off) {
    uintptr_t a = p + off;
    return (a < 0x10000) ? 0 : *reinterpret_cast<uintptr_t *>(a);
}
ID3D11Texture2D *rt_texture(uintptr_t cam, uintptr_t rt_off) {
    uintptr_t rt = deref(cam, rt_off);
    if (!rt) return nullptr;
    uintptr_t kid = deref(rt, OFF_RT_KIDCOLOR);
    if (!kid) return nullptr;
    return reinterpret_cast<ID3D11Texture2D *>(deref(kid, OFF_KID_TEX));
}

// Row-major, row-vector: (a*b)[r][c] = sum_k a[r][k]*b[k][c].
void mat_mul(const float *a, const float *b, float *o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
}
// returns false if singular.
bool mat_inverse_d(const float *m, double *o) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) { a[r][c] = m[r * 4 + c]; a[r][c + 4] = (r == c) ? 1.0 : 0.0; }
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++) if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-12) return false;
        for (int c = 0; c < 8; c++) { double t = a[col][c]; a[col][c] = a[piv][c]; a[piv][c] = t; }
        double d = a[col][col];
        for (int c = 0; c < 8; c++) a[col][c] /= d;
        for (int r = 0; r < 4; r++) if (r != col) {
            double f = a[r][col];
            for (int c = 0; c < 8; c++) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[r * 4 + c] = a[r][c + 4];
    return true;
}

bool mat_inverse(const float *m, float *o) {
    double d[16];
    if (!mat_inverse_d(m, d)) return false;
    for (int i = 0; i < 16; i++) o[i] = (float)d[i];
    return true;
}

// Compose the clip(cur)->clip(prev) reprojection matrix ENTIRELY IN DOUBLE:
// M = invVPcur * VPprev. AC's world coordinates are hundreds of metres from the origin,
// so an fp32 world-space round trip loses ~0.5px to cancellation noise (measured on a
// static camera); composed in double the identical-matrix case collapses to the exact
// identity and real motion survives to full precision.
// I'll take your fucking word for it Claude, this shit is gnarly
bool reproj_matrix(const float *vp_cur, const float *vp_prev, float *out) {
    double inv[16];
    if (!mat_inverse_d(vp_cur, inv)) return false;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += inv[r * 4 + k] * (double)vp_prev[k * 4 + c];
            out[r * 4 + c] = (float)s;
        }
    return true;
}

bool make_tex(ID3D11Device *dev, unsigned w, unsigned h, DXGI_FORMAT fmt,
              ID3D11Texture2D **tex, ID3D11UnorderedAccessView **uav) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, tex))) return false;
    return SUCCEEDED(dev->CreateUnorderedAccessView(*tex, nullptr, uav));
}

bool init_resources(ID3D11Device *dev, uintptr_t cam, unsigned w, unsigned h) {
    g_w = w; g_h = h;
    if (FAILED(dev->CreateComputeShader(g_reproject_cs, sizeof(g_reproject_cs), nullptr, &g_cs))) {
        acre_log("  dlss: CreateComputeShader failed"); return false;
    }
    if (!make_tex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, &g_mv, &g_mv_uav)) {
        acre_log("  dlss: MV texture failed"); return false;
    }
    if (!make_tex(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, &g_out, &g_out_uav)) {
        acre_log("  dlss: output texture failed"); return false;
    }
    // SRV on AC's resolved depth (R32_FLOAT) for the reproject shader.
    ID3D11Texture2D *depth = rt_texture(cam, OFF_SCV_RTDEPTH);
    if (!depth) { acre_log("  dlss: no depth tex"); return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(depth, &sd, &g_depth_srv))) {
        acre_log("  dlss: depth SRV failed"); return false;
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb))) { acre_log("  dlss: cbuffer failed"); return false; }

    D3D11_TEXTURE2D_DESC rd = {};
    rd.Width = w; rd.Height = h; rd.MipLevels = 1; rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; rd.SampleDesc.Count = 1;
    rd.Usage = D3D11_USAGE_STAGING; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture2D(&rd, nullptr, &g_readback))) { acre_log("  dlss: readback failed"); return false; }

    acre_log("  dlss: resources ready %ux%u", w, h);
    return true;
}

float half_to_float(unsigned short h) {
    unsigned sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    unsigned f;
    if (exp == 0) { if (man == 0) f = sign << 31; else { exp = 127 - 15 + 1;
        while (!(man & 0x400)) { man <<= 1; exp--; } man &= 0x3FF; f = (sign << 31) | (exp << 23) | (man << 13); } }
    else if (exp == 0x1F) f = (sign << 31) | (0xFF << 23) | (man << 13);
    else f = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
    float out; memcpy(&out, &f, 4); return out;
}

void readback_stats(ID3D11DeviceContext *ctx) {
    ctx->CopyResource(g_readback, g_out);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_readback, 0, D3D11_MAP_READ, 0, &m))) { acre_log("  dlss: readback Map failed"); return; }
    double sum = 0; float mn = 1e30f, mx = -1e30f; int nz = 0, nan = 0, n = 0;
    for (unsigned y = 0; y < g_h; y += 37) {
        const unsigned short *row = (const unsigned short *)((const BYTE *)m.pData + (size_t)y * m.RowPitch);
        for (unsigned x = 0; x < g_w; x += 37) {
            float r = half_to_float(row[x * 4]);
            if (r != r) nan++; else { sum += r; if (r < mn) mn = r; if (r > mx) mx = r; if (r > 1e-6f) nz++; }
            n++;
        }
    }
    ctx->Unmap(g_readback, 0);
    acre_log("  dlss: OUTPUT R: mean=%.5f min=%.5f max=%.5f nonzero=%d/%d nan=%d",
             n ? sum / n : 0.0, mn, mx, nz, n, nan);
}

}  // namespace

namespace {
ID3D11Texture2D *g_ip_out = nullptr;   ID3D11UnorderedAccessView *g_ip_out_uav = nullptr;
ID3D11Texture2D *g_ip_mv = nullptr;    ID3D11UnorderedAccessView *g_ip_mv_uav = nullptr;
ID3D11Buffer *g_ip_cb = nullptr;
ID3D11ComputeShader *g_ip_cs = nullptr;
ID3D11ShaderResourceView *g_ip_depth_srv = nullptr;
void *g_ip_depth_for = nullptr;        // depth texture the SRV was made for
unsigned g_ip_w = 0, g_ip_h = 0;
bool g_ip_have_prev[2] = {false, false};
float g_ip_vp_prev[2][16];

bool ip_init(ID3D11Device *dev, ID3D11Texture2D *color) {
    D3D11_TEXTURE2D_DESC cd; color->GetDesc(&cd);
    g_ip_w = cd.Width; g_ip_h = cd.Height;
    if (FAILED(dev->CreateComputeShader(g_reproject_cs, sizeof(g_reproject_cs), nullptr, &g_ip_cs)))
        { acre_log("  ip: CS failed"); return false; }
    if (!make_tex(dev, g_ip_w, g_ip_h, DXGI_FORMAT_R16G16_FLOAT, &g_ip_mv, &g_ip_mv_uav))
        { acre_log("  ip: mv failed"); return false; }
    if (!make_tex(dev, g_ip_w, g_ip_h, cd.Format, &g_ip_out, &g_ip_out_uav))
        { acre_log("  ip: out failed (fmt %d)", cd.Format); return false; }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_ip_cb))) { acre_log("  ip: cb failed"); return false; }
    acre_log("  ip: resources ready %ux%u fmt=%d", g_ip_w, g_ip_h, cd.Format);
    return true;
}

bool ip_depth_srv(ID3D11Device *dev, ID3D11Texture2D *depth) {
    if (g_ip_depth_srv && g_ip_depth_for == depth) return true;
    if (g_ip_depth_srv) { g_ip_depth_srv->Release(); g_ip_depth_srv = nullptr; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_FLOAT;             // typeless depth -> float
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(depth, &sd, &g_ip_depth_srv))) return false;
    g_ip_depth_for = depth;
    return true;
}
}  // namespace

// Run reproject + DLSS DLAA on CSP's per-eye scene color+depth, in place: the DLSS'd HDR
// is copied back into `color` so CSP's own post-processing tonemaps it into the eye.
// Caller must wrap this in d3d11_backup/restore.
extern "C" void acre_dlss_inplace(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                  ID3D11Texture2D *color, ID3D11Texture2D *depth,
                                  int eye, uintptr_t cam) {
    if (!acre_ngx_handle_eye(eye) || eye < 0 || eye > 1) return;

    static int noeval = -1;
    if (noeval < 0) noeval = GetEnvironmentVariableA("ACRE_NOEVAL", nullptr, 0) ? 1 : 0;
    if (noeval) return;

    static int tint = -1;
    if (tint < 0) { char b[8]; tint = GetEnvironmentVariableA("ACRE_TINT", b, 8) ? 1 : 0; }
    if (tint) {
        ID3D11RenderTargetView *rtv = nullptr;
        if (SUCCEEDED(dev->CreateRenderTargetView(color, nullptr, &rtv)) && rtv) {
            const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
            ctx->ClearRenderTargetView(rtv, magenta);
            rtv->Release();
        }
        return;
    }

    static long dbg = 0;
    if (dbg++ < 2) {
        D3D11_TEXTURE2D_DESC cd, dd; color->GetDesc(&cd); depth->GetDesc(&dd);
        acre_log("  ip: color fmt=%d samples=%u bind=0x%x | depth fmt=%d samples=%u bind=0x%x",
                 cd.Format, cd.SampleDesc.Count, cd.BindFlags,
                 dd.Format, dd.SampleDesc.Count, dd.BindFlags);
        acre_log("  MEASURE dlaa eye=%d: scene %ux%u -> DLSS in-place %ux%u (native, 1:1 — no upscale in dlaa mode)",
                 eye, cd.Width, cd.Height, cd.Width, cd.Height);
    }
    if (!g_ip_cs && !ip_init(dev, color)) return;
    if (!ip_depth_srv(dev, depth)) { acre_log("  ip: depth SRV failed"); return; }

    float view[16], proj[16], vp[16];
    memcpy(view, reinterpret_cast<void *>(cam + 2512), 64);                 // viewMatrix
    memcpy(proj, reinterpret_cast<void *>(cam + 1968 + eye * 176 + 72), 64); // viveData[eye].eyeProjection
    mat_mul(view, proj, vp);

    static int matlog = -1;
    if (matlog < 0) matlog = GetEnvironmentVariableA("ACRE_MATLOG", nullptr, 0) ? 1 : 0;
    if (matlog && g_ip_have_prev[eye]) {
        static long mln[2] = {0, 0};
        if (++mln[eye] <= 12 || (mln[eye] % 300) == 0) {
            float dv = 0;
            for (int i = 0; i < 16; i++) {
                float d = vp[i] - g_ip_vp_prev[eye][i];
                if (d < 0) d = -d;
                if (d > dv) dv = d;
            }
            acre_log("  MATLOG eye=%d dVPmax=%.6f viewT=(%.4f %.4f %.4f)",
                     eye, dv, view[12], view[13], view[14]);
        }
    }

    CB cb;
    if (!reproj_matrix(vp, g_ip_have_prev[eye] ? g_ip_vp_prev[eye] : vp, cb.reproj)) return;
    cb.dim[0] = (float)g_ip_w; cb.dim[1] = (float)g_ip_h; cb.pad[0] = cb.pad[1] = 0;
    D3D11_MAPPED_SUBRESOURCE mm;
    if (SUCCEEDED(ctx->Map(g_ip_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mm))) {
        memcpy(mm.pData, &cb, sizeof(cb)); ctx->Unmap(g_ip_cb, 0);
    }
    memcpy(g_ip_vp_prev[eye], vp, 64); g_ip_have_prev[eye] = true;

    static int mvzero = -1;
    if (mvzero < 0) mvzero = GetEnvironmentVariableA("ACRE_MVZERO", nullptr, 0) ? 1 : 0;
    if (mvzero) {
        const FLOAT z4[4] = {0, 0, 0, 0};
        ctx->ClearUnorderedAccessViewFloat(g_ip_mv_uav, z4);
    } else {
        ctx->CSSetShader(g_ip_cs, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &g_ip_cb);
        ctx->CSSetShaderResources(0, 1, &g_ip_depth_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_ip_mv_uav, nullptr);
        ctx->Dispatch((g_ip_w + 7) / 8, (g_ip_h + 7) / 8, 1);
        ID3D11UnorderedAccessView *nul = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &nul, nullptr);
    }

    static int mvstats = -1;
    if (mvstats < 0) mvstats = GetEnvironmentVariableA("ACRE_MVSTATS", nullptr, 0) ? 1 : 0;
    if (mvstats && eye == 0) {
        static long mvn = 0;
        if ((++mvn % 300) == 1) {
            static ID3D11Texture2D *stg = nullptr;
            if (!stg) {
                D3D11_TEXTURE2D_DESC sd = {};
                sd.Width = g_ip_w; sd.Height = g_ip_h; sd.MipLevels = 1; sd.ArraySize = 1;
                sd.Format = DXGI_FORMAT_R16G16_FLOAT; sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                dev->CreateTexture2D(&sd, nullptr, &stg);
            }
            if (stg) {
                ctx->CopyResource(stg, g_ip_mv);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) {
                    double sum = 0; float mx = 0; int n = 0, nz = 0;
                    for (unsigned y = 0; y < g_ip_h; y += 37) {
                        const unsigned short *row =
                            (const unsigned short *)((const BYTE *)m.pData + (size_t)y * m.RowPitch);
                        for (unsigned x = 0; x < g_ip_w; x += 37) {
                            float mvx = half_to_float(row[x * 2]), mvy = half_to_float(row[x * 2 + 1]);
                            float mag = sqrtf(mvx * mvx + mvy * mvy);
                            sum += mag; if (mag > mx) mx = mag; if (mag > 0.01f) nz++; n++;
                        }
                    }
                    ctx->Unmap(stg, 0);
                    acre_log("  MVSTATS eye0: mean=%.4f max=%.4f nonzero=%d/%d (px)",
                             n ? sum / n : 0.0, mx, nz, n);
                }
            }
        }
    }

    NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = g_ip_out;
    ep.pInDepth = depth;
    ep.pInMotionVectors = g_ip_mv;
    float jx = 0, jy = 0;
    reported_jitter(&jx, &jy);
    ep.InJitterOffsetX = jx;
    ep.InJitterOffsetY = jy;
    ep.InRenderSubrectDimensions.Width = g_ip_w;
    ep.InRenderSubrectDimensions.Height = g_ip_h;
    ep.InReset = 0;
    ep.InMVScaleX = mv_scale();
    ep.InMVScaleY = mv_scale();

    NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(
        ctx, (NVSDK_NGX_Handle *)acre_ngx_handle_eye(eye),
        (NVSDK_NGX_Parameter *)acre_ngx_eval_params_eye(eye), &ep);
    if (!NVSDK_NGX_FAILED(r)) {
        float sharp = acre_cfg_sharpness();
        static ID3D11ShaderResourceView *out_srv = nullptr;
        static ID3D11Texture2D *sharp_tex = nullptr;
        static ID3D11UnorderedAccessView *sharp_uav = nullptr;
        if (sharp > 0.0f && !out_srv)
            dev->CreateShaderResourceView(g_ip_out, nullptr, &out_srv);
        if (sharp > 0.0f && !sharp_tex)
            make_tex(dev, g_ip_w, g_ip_h, DXGI_FORMAT_R16G16B16A16_FLOAT, &sharp_tex, &sharp_uav);
        if (sharp > 0.0f && out_srv && sharp_uav) {
            sharpen_pass(dev, ctx, out_srv, sharp_uav, g_ip_w, g_ip_h, sharp, 1 /*hdr*/);
            ctx->CopyResource(color, sharp_tex);  // DLSS'd + sharpened HDR back to CSP
        } else {
            ctx->CopyResource(color, g_ip_out);   // feed DLSS'd HDR back into CSP's pipeline
        }
    }

    static volatile LONG evals[2] = {0, 0};
    LONG n = InterlockedIncrement(&evals[eye]);
    if (eye == 1 && (n % 600) == 0)
        acre_log("  ip: eval counts eye0=%ld eye1=%ld (last=0x%08X)", evals[0], evals[1], (unsigned)r);

    static long once = 0;
    if (once++ < 8) {
        float shared_m31 = *reinterpret_cast<float *>(cam + 2448 + 8 * 4);
        acre_log("  ip: eye=%d  sharedProj M31=%+.3f  -> 0x%08X", eye, shared_m31, (unsigned)r);
    }
}

namespace {
ID3D11Texture2D *g_us_mv = nullptr;    ID3D11UnorderedAccessView *g_us_mv_uav = nullptr;
ID3D11Buffer *g_us_cb = nullptr;
ID3D11ComputeShader *g_us_cs = nullptr;
ID3D11ShaderResourceView *g_us_depth_srv = nullptr;
void *g_us_depth_for = nullptr;
unsigned g_us_w = 0, g_us_h = 0;
bool g_us_have_prev[2] = {false, false};
float g_us_vp_prev[2][16];
// Reactive-mask resources (HUD de-ghost).
ID3D11ComputeShader *g_us_react_cs = nullptr;
ID3D11Texture2D *g_us_mask = nullptr;  ID3D11UnorderedAccessView *g_us_mask_uav = nullptr;
ID3D11ShaderResourceView *g_us_mask_srv = nullptr, *g_us_mv_srv = nullptr;
ID3D11Buffer *g_us_mask_cb = nullptr;
ID3D11Texture2D *g_us_prevcol[2] = {nullptr, nullptr};   // previous renderEye per eye
ID3D11ShaderResourceView *g_us_prev_srv[2] = {nullptr, nullptr};
ID3D11ShaderResourceView *g_us_cur_srv[2] = {nullptr, nullptr};
void *g_us_cur_for[2] = {nullptr, nullptr};
struct MaskCB { float dim[2]; float pad[2]; };

bool us_init(ID3D11Device *dev, unsigned w, unsigned h) {
    g_us_w = w; g_us_h = h;
    if (FAILED(dev->CreateComputeShader(g_reproject_cs, sizeof(g_reproject_cs), nullptr, &g_us_cs))) return false;
    if (FAILED(dev->CreateComputeShader(g_reactive_mask_cs, sizeof(g_reactive_mask_cs), nullptr, &g_us_react_cs))) return false;
    if (!make_tex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, &g_us_mv, &g_us_mv_uav)) return false;
    if (!make_tex(dev, w, h, DXGI_FORMAT_R8_UNORM, &g_us_mask, &g_us_mask_uav)) return false;
    dev->CreateShaderResourceView(g_us_mv, nullptr, &g_us_mv_srv);
    dev->CreateShaderResourceView(g_us_mask, nullptr, &g_us_mask_srv);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_us_cb))) return false;
    D3D11_BUFFER_DESC mb = {};
    mb.ByteWidth = sizeof(MaskCB); mb.Usage = D3D11_USAGE_DYNAMIC;
    mb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; mb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateBuffer(&mb, nullptr, &g_us_mask_cb));
}

// Reactive mask from current vs reprojected-previous renderEye. Returns the mask texture
// to hand DLSS, or nullptr on the first frame (no previous). Also stores current as prev.
ID3D11Texture2D *us_reactive(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                             ID3D11Texture2D *color, int eye) {
    if (!acre_cfg_reactive()) return nullptr;
    if (!g_us_cur_srv[eye] || g_us_cur_for[eye] != color) {
        if (g_us_cur_srv[eye]) g_us_cur_srv[eye]->Release();
        if (FAILED(dev->CreateShaderResourceView(color, nullptr, &g_us_cur_srv[eye]))) return nullptr;
        g_us_cur_for[eye] = color;
    }
    if (!g_us_prevcol[eye]) {
        D3D11_TEXTURE2D_DESC d; color->GetDesc(&d);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_us_prevcol[eye]))) return nullptr;
        dev->CreateShaderResourceView(g_us_prevcol[eye], nullptr, &g_us_prev_srv[eye]);
        ctx->CopyResource(g_us_prevcol[eye], color);   // seed; no valid prev yet
        return nullptr;
    }
    MaskCB mc = {(float)g_us_w, (float)g_us_h, acre_cfg_mask_scale(), 0};
    D3D11_MAPPED_SUBRESOURCE mm;
    if (SUCCEEDED(ctx->Map(g_us_mask_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mm))) {
        memcpy(mm.pData, &mc, sizeof(mc)); ctx->Unmap(g_us_mask_cb, 0);
    }
    ID3D11ShaderResourceView *srvs[3] = {g_us_cur_srv[eye], g_us_prev_srv[eye], g_us_mv_srv};
    ctx->CSSetShader(g_us_react_cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_us_mask_cb);
    ctx->CSSetShaderResources(0, 3, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_us_mask_uav, nullptr);
    ctx->Dispatch((g_us_w + 7) / 8, (g_us_h + 7) / 8, 1);
    ID3D11UnorderedAccessView *nul = nullptr; ctx->CSSetUnorderedAccessViews(0, 1, &nul, nullptr);
    ID3D11ShaderResourceView *nsrv[3] = {nullptr, nullptr, nullptr};
    ctx->CSSetShaderResources(0, 3, nsrv);
    ctx->CopyResource(g_us_prevcol[eye], color);       // for next frame
    return g_us_mask;
}
bool us_depth_srv(ID3D11Device *dev, ID3D11Texture2D *depth) {
    if (g_us_depth_srv && g_us_depth_for == depth) return true;
    if (g_us_depth_srv) { g_us_depth_srv->Release(); g_us_depth_srv = nullptr; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(depth, &sd, &g_us_depth_srv))) return false;
    g_us_depth_for = depth; return true;
}
}  // namespace

// Reproject at input res + DLSS-upscale color(inW×inH) -> out(outW×outH). 
extern "C" void acre_dlss_upscale(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                  ID3D11Texture2D *color, ID3D11Texture2D *depth,
                                  int eye, ID3D11Texture2D *out, uintptr_t cam) {
    if (!acre_ngx_up_handle(eye) || eye < 0 || eye > 1 || !color || !depth || !out) return;
    D3D11_TEXTURE2D_DESC cd; color->GetDesc(&cd);
    if (!g_us_cs && !us_init(dev, cd.Width, cd.Height)) return;
    if (!us_depth_srv(dev, depth)) return;

    float view[16], proj[16], vp[16];
    memcpy(view, reinterpret_cast<void *>(cam + 2512), 64);
    memcpy(proj, reinterpret_cast<void *>(cam + 1968 + eye * 176 + 72), 64);
    mat_mul(view, proj, vp);

    CB cb;
    if (!reproj_matrix(vp, g_us_have_prev[eye] ? g_us_vp_prev[eye] : vp, cb.reproj)) return;
    cb.dim[0] = (float)g_us_w; cb.dim[1] = (float)g_us_h; cb.pad[0] = cb.pad[1] = 0;
    D3D11_MAPPED_SUBRESOURCE mm;
    if (SUCCEEDED(ctx->Map(g_us_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mm))) {
        memcpy(mm.pData, &cb, sizeof(cb)); ctx->Unmap(g_us_cb, 0);
    }
    memcpy(g_us_vp_prev[eye], vp, 64); g_us_have_prev[eye] = true;

    ctx->CSSetShader(g_us_cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_us_cb);
    ctx->CSSetShaderResources(0, 1, &g_us_depth_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_us_mv_uav, nullptr);
    ctx->Dispatch((g_us_w + 7) / 8, (g_us_h + 7) / 8, 1);
    ID3D11UnorderedAccessView *nul = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &nul, nullptr);
    ID3D11ShaderResourceView *nsrv = nullptr; ctx->CSSetShaderResources(0, 1, &nsrv);

    // Reactive mask
    ID3D11Texture2D *mask = us_reactive(dev, ctx, color, eye);

    // Optional sharpen
    float sharp = acre_cfg_sharpness();
    static ID3D11Texture2D *raw[2] = {nullptr, nullptr};
    static ID3D11ShaderResourceView *raw_srv[2] = {nullptr, nullptr};
    static ID3D11UnorderedAccessView *out_uav[2] = {nullptr, nullptr};
    static void *out_for[2] = {nullptr, nullptr};
    D3D11_TEXTURE2D_DESC od; out->GetDesc(&od);
    ID3D11Texture2D *target = out;
    if (sharp > 0.0f) {
        if (!raw[eye] && SUCCEEDED(dev->CreateTexture2D(&od, nullptr, &raw[eye])) && raw[eye])
            dev->CreateShaderResourceView(raw[eye], nullptr, &raw_srv[eye]);
        if (out_for[eye] != out) {
            if (out_uav[eye]) { out_uav[eye]->Release(); out_uav[eye] = nullptr; }
            dev->CreateUnorderedAccessView(out, nullptr, &out_uav[eye]);
            out_for[eye] = out;
        }
        if (raw[eye] && raw_srv[eye] && out_uav[eye]) target = raw[eye];
    }

    float jx = 0, jy = 0; reported_jitter(&jx, &jy);
    NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = target;
    ep.pInDepth = depth;
    ep.pInMotionVectors = g_us_mv;
    ep.pInBiasCurrentColorMask = mask;
    ep.InJitterOffsetX = jx; ep.InJitterOffsetY = jy;
    ep.InRenderSubrectDimensions.Width = g_us_w;
    ep.InRenderSubrectDimensions.Height = g_us_h;
    ep.InReset = 0; ep.InMVScaleX = mv_scale(); ep.InMVScaleY = mv_scale();

    NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(
        ctx, (NVSDK_NGX_Handle *)acre_ngx_up_handle(eye),
        (NVSDK_NGX_Parameter *)acre_ngx_up_params(eye), &ep);
    if (!NVSDK_NGX_FAILED(r) && target != out)
        sharpen_pass(dev, ctx, raw_srv[eye], out_uav[eye], od.Width, od.Height, sharp, 0 /*ldr*/);
    static long once = 0;
    if (once++ < 4) acre_log("  upscale: eye=%d -> 0x%08X (%ls)", eye, (unsigned)r, GetNGXResultAsString(r));
}

// Called every frame from the Present hook
extern "C" void acre_dlss_frame(ID3D11Device *dev, ID3D11DeviceContext *ctx, uintptr_t cam) {
    if (!acre_ngx_handle()) return;
    if (!g_ready) {
        unsigned w = *reinterpret_cast<int *>(cam + 3944);
        unsigned h = *reinterpret_cast<int *>(cam + 3948);
        g_ready = init_resources(dev, cam, w, h);
        if (!g_ready) return;
    }
    g_frame++;

    ID3D11Texture2D *color = rt_texture(cam, OFF_SCV_RTRESOLV);
    ID3D11Texture2D *depth = rt_texture(cam, OFF_SCV_RTDEPTH);
    if (!color || !depth) return;

    // Current VP from the live matrices.
    float view[16], proj[16], vp[16];
    memcpy(view, reinterpret_cast<void *>(cam + OFF_SCV_VIEW), 64);
    memcpy(proj, reinterpret_cast<void *>(cam + OFF_SCV_PROJ), 64);
    mat_mul(view, proj, vp);

    // Reproject: fill motion vectors. First frame has no previous VP -> zero motion.
    CB cb;
    if (!reproj_matrix(vp, g_have_prev ? g_vp_prev : vp, cb.reproj)) return;
    cb.dim[0] = (float)g_w; cb.dim[1] = (float)g_h; cb.pad[0] = cb.pad[1] = 0;
    D3D11_MAPPED_SUBRESOURCE mm;
    if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mm))) {
        memcpy(mm.pData, &cb, sizeof(cb)); ctx->Unmap(g_cb, 0);
    }
    ctx->CSSetShader(g_cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ctx->CSSetShaderResources(0, 1, &g_depth_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_mv_uav, nullptr);
    ctx->Dispatch((g_w + 7) / 8, (g_h + 7) / 8, 1);
    ID3D11UnorderedAccessView *nulluav = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &nulluav, nullptr);

    memcpy(g_vp_prev, vp, 64); g_have_prev = true;

    // DLSS evaluate DLAA: color+depth+mv -> output.
    NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = g_out;
    ep.pInDepth = depth;
    ep.pInMotionVectors = g_mv;
    ep.InJitterOffsetX = 0.0f;
    ep.InJitterOffsetY = 0.0f;
    ep.InRenderSubrectDimensions.Width = g_w;
    ep.InRenderSubrectDimensions.Height = g_h;
    ep.InReset = (g_frame == 1) ? 1 : 0;
    ep.InMVScaleX = 1.0f;           // motion vectors already in pixels
    ep.InMVScaleY = 1.0f;

    NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(
        ctx, (NVSDK_NGX_Handle *)acre_ngx_handle(),
        (NVSDK_NGX_Parameter *)acre_ngx_eval_params(), &ep);

    if (g_frame == 3 || g_frame == 200) {
        acre_log("  dlss: Evaluate frame %ld -> 0x%08X (%ls)",
                 g_frame, (unsigned)r, GetNGXResultAsString(r));
        if (!NVSDK_NGX_FAILED(r)) readback_stats(ctx);
    }
}
