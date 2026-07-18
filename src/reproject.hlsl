// SPDX-License-Identifier: GPL-3.0-or-later
// reproject.hlsl — camera motion vectors from depth, per eye.
//
// One clip->clip matrix (invVPcur * VPprev), composed on the CPU in double precision.
// Don't be tempted to round-trip through world space in here: AC tracks sit hundreds of
// metres from the origin and fp32 cancellation alone puts ~0.5px of phantom motion on
// every pixel, which quietly ruins DLSS.
//
// Static geometry and camera motion only; other cars ghost slightly. Output is pixels,
// (prev - cur), so DLSS gets InMVScale=(1,1).

cbuffer ReprojectCB : register(b0)
{
    row_major float4x4 gReproj;   // clip(cur) -> clip(prev), composed in double on CPU
    float2 gRenderDim;            // (width, height) in pixels
    float2 gPad;
};

Texture2D<float>    gDepth  : register(t0);
RWTexture2D<float2> gMotion : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gRenderDim.x || tid.y >= (uint)gRenderDim.y)
        return;

    uint2 px = tid.xy;
    float2 uv = (float2(px) + 0.5f) / gRenderDim;
    float ndcz = gDepth.Load(int3(px, 0));

    float4 clipCur = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, ndcz, 1.0f);
    float4 clipPrev = mul(clipCur, gReproj);
    float2 prevUv;
    if (clipPrev.w > 1e-6f)
        prevUv = float2((clipPrev.x / clipPrev.w) * 0.5f + 0.5f,
                        0.5f - (clipPrev.y / clipPrev.w) * 0.5f);
    else
        prevUv = uv;   // behind previous camera; no meaningful motion

    gMotion[px] = (prevUv - uv) * gRenderDim;   // render-pixel units, prev - cur
}
