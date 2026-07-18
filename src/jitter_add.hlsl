// SPDX-License-Identifier: GPL-3.0-or-later
// jitter_add.hlsl — adds sub-pixel jitter to a copy of CSP's transform CB.
// CopyResource(work, cspCB) -> this -> CopyResource(shadow, work). Adding to the live
// values keeps the real FOV and per-eye frustum intact no matter what.
// CB layout: view@[0..15], projection@[16..31]; jitter goes to M[0][2] (byte 72) and
// M[1][2] (byte 88).

cbuffer JitterCB : register(b0)
{
    float2 gJitterNdc;   // (-2*jx_px/w, -2*jy_px/h)
    float2 gPad;
};

RWByteAddressBuffer gBuf : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    float m02 = asfloat(gBuf.Load(72));
    float m12 = asfloat(gBuf.Load(88));
    gBuf.Store(72, asuint(m02 + gJitterNdc.x));
    gBuf.Store(88, asuint(m12 + gJitterNdc.y));
}
