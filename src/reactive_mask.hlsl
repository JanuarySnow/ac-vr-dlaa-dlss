// SPDX-License-Identifier: GPL-3.0-or-later
// reactive_mask.hlsl — auto-reactive mask for DLSS upscaling (HUD de-ghosting).
//
// For each pixel, reproject the previous frame's colour through the motion vector and
// compare to the current colour. Where they disagree, the motion vector is wrong for that
// pixel - screen-locked HUD/overlays (scene MVs move them incorrectly) and genuine moving
// objects. Both should be "reactive": DLSS is told to favour the current frame there via
// pInBiasCurrentColorMask, which removes the HUD ghosting seen when upscaling the final
// composited image.

cbuffer MaskCB : register(b0)
{
    float2 gDim;      // input resolution (pixels)
    float  gScale;    // reactive sensitivity (config mask_scale)
    float  gPad;
};

Texture2D<float4>   gCur  : register(t0);   // current renderEye (LDR)
Texture2D<float4>   gPrev : register(t1);   // previous renderEye
Texture2D<float2>   gMV   : register(t2);   // motion, render pixels, prev - cur
RWTexture2D<float>  gMask : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gDim.x || tid.y >= (uint)gDim.y)
        return;
    uint2 px = tid.xy;

    float2 mv = gMV.Load(int3(px, 0));                 // pixels, points to prev location
    int2 prevPx = int2(px) + int2(round(mv.x), round(mv.y));
    prevPx = clamp(prevPx, int2(0, 0), int2((int)gDim.x - 1, (int)gDim.y - 1));

    float3 cur  = gCur.Load(int3(px, 0)).rgb;
    float3 prev = gPrev.Load(int3(prevPx, 0)).rgb;

    // Disagreement -> reactive. Scale tuned so small temporal noise stays 0ish while HUD
    // and fast motion saturate to 1.
    float d = length(cur - prev);
    gMask[px] = saturate(d * gScale);
}
