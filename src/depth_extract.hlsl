// SPDX-License-Identifier: GPL-3.0-or-later
// depth_extract.hlsl — copy one eye's half of the double-wide SPS depth into a per-eye
// R32_FLOAT temp. D3D11 forbids CopySubresourceRegion with a source box on a depth-stencil
// resource (the copy silently no-ops and the destination stays zero), so under SPS the depth
// MUST be extracted in a shader. Reading a depth resource through an SRV is allowed.
Texture2D<float>   gSrc : register(t0);   // double-wide scene depth (SRV, R32 view)
RWTexture2D<float> gDst : register(u0);   // per-eye R32_FLOAT temp (UAV)

cbuffer ExtractCB : register(b0)
{
    int gXOff;   // eye * subW
    int gW;      // per-eye width
    int gH;      // height
    int gPad;
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if ((int)tid.x >= gW || (int)tid.y >= gH) return;
    gDst[tid.xy] = gSrc.Load(int3(gXOff + (int)tid.x, (int)tid.y, 0));
}
