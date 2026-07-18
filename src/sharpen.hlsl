// SPDX-License-Identifier: GPL-3.0-or-later
// sharpen.hlsl — CAS-style adaptive sharpen, run right after DLSS (which dropped its own
// sharpener in 2.5.1). Per-pixel weight backs off at strong edges so no halos. gHdr=1
// wraps the kernel in Reinhard so pre-tonemap HDR input doesn't blow out.

cbuffer SharpenCB : register(b0)
{
    float2 gDim;         // texture size in pixels
    float  gSharpness;   // 0..1 (0 = pass disabled on CPU side)
    float  gHdr;         // 1 = Reinhard-wrap the kernel (HDR input)
};

Texture2D<float4>   gIn  : register(t0);
RWTexture2D<float4> gOut : register(u0);

float3 fetch(int2 p)
{
    p = clamp(p, int2(0, 0), int2(gDim) - 1);
    float3 c = gIn.Load(int3(p, 0)).rgb;
    if (gHdr > 0.5)
        c = c / (1.0 + c);          // Reinhard -> [0,1)
    return c;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gDim.x || tid.y >= (uint)gDim.y)
        return;
    int2 p = int2(tid.xy);

    float3 e = fetch(p);
    float3 b = fetch(p + int2(0, -1));
    float3 d = fetch(p + int2(-1, 0));
    float3 f = fetch(p + int2(1, 0));
    float3 h = fetch(p + int2(0, 1));

    // Adaptive amount: high where local contrast is low (texture), low at strong edges
    float3 mn = min(e, min(min(b, d), min(f, h)));
    float3 mx = max(e, max(max(b, d), max(f, h)));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    float peak = -1.0 / lerp(8.0, 5.0, saturate(gSharpness));
    float3 w = amp * peak;                               // negative cross weights

    float3 res = (e + (b + d + f + h) * w) / (1.0 + 4.0 * w);
    res = clamp(res, 0.0, gHdr > 0.5 ? 0.9999 : 1.0);
    if (gHdr > 0.5)
        res = res / (1.0 - res);    // inverse Reinhard back to HDR

    gOut[p] = float4(res, gIn.Load(int3(p, 0)).a);
}
