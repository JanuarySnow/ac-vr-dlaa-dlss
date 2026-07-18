// SPDX-License-Identifier: GPL-3.0-or-later
// ctx_backup.h — save/restore D3D11 immediate-context state around an injected pass.
//
// DLSS evaluat rewrite large parts of the pipeline. When we
// inject mid-frame into CSP's rendering, CSP's following draws would inherit our state
// unless we put everything back.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

struct D3D11StateBackup {
    // IA
    ID3D11InputLayout *ia_layout;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *ia_index; DXGI_FORMAT ia_index_fmt; UINT ia_index_off;
    ID3D11Buffer *ia_vb[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    UINT ia_vb_stride[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    UINT ia_vb_off[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    // RS
    ID3D11RasterizerState *rs;
    UINT nvp; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nsc; D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    // OM
    ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *dsv;
    ID3D11BlendState *blend; FLOAT blend_factor[4]; UINT sample_mask;
    ID3D11DepthStencilState *dss; UINT stencil_ref;
    // VS / PS
    ID3D11VertexShader *vs; ID3D11PixelShader *ps;
    ID3D11Buffer *vs_cb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
    ID3D11Buffer *ps_cb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
    ID3D11ShaderResourceView *ps_srv[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    ID3D11SamplerState *ps_samp[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    // CS
    ID3D11ComputeShader *cs;
    ID3D11Buffer *cs_cb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
    ID3D11ShaderResourceView *cs_srv[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    ID3D11UnorderedAccessView *cs_uav[D3D11_1_UAV_SLOT_COUNT];
    ID3D11SamplerState *cs_samp[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
};

static inline void d3d11_backup(ID3D11DeviceContext *c, D3D11StateBackup *b) {
    ZeroMemory(b, sizeof(*b));
    c->IAGetInputLayout(&b->ia_layout);
    c->IAGetPrimitiveTopology(&b->topo);
    c->IAGetIndexBuffer(&b->ia_index, &b->ia_index_fmt, &b->ia_index_off);
    c->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, b->ia_vb, b->ia_vb_stride, b->ia_vb_off);
    c->RSGetState(&b->rs);
    b->nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetViewports(&b->nvp, b->vp);
    b->nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetScissorRects(&b->nsc, b->sc);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b->rtv, &b->dsv);
    c->OMGetBlendState(&b->blend, b->blend_factor, &b->sample_mask);
    c->OMGetDepthStencilState(&b->dss, &b->stencil_ref);
    c->VSGetShader(&b->vs, nullptr, nullptr);
    c->PSGetShader(&b->ps, nullptr, nullptr);
    c->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, b->vs_cb);
    c->PSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, b->ps_cb);
    c->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, b->ps_srv);
    c->PSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, b->ps_samp);
    c->CSGetShader(&b->cs, nullptr, nullptr);
    c->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, b->cs_cb);
    c->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, b->cs_srv);
    c->CSGetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, b->cs_uav);
    c->CSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, b->cs_samp);
}

// Restore, then Release the references we got
template <class T> static inline void rel(T *&p) { if (p) { p->Release(); p = nullptr; } }
template <class T, int N> static inline void rel_arr(T *(&a)[N]) { for (int i = 0; i < N; i++) rel(a[i]); }

static inline void d3d11_restore(ID3D11DeviceContext *c, D3D11StateBackup *b) {
    c->IASetInputLayout(b->ia_layout);
    c->IASetPrimitiveTopology(b->topo);
    c->IASetIndexBuffer(b->ia_index, b->ia_index_fmt, b->ia_index_off);
    c->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, b->ia_vb, b->ia_vb_stride, b->ia_vb_off);
    c->RSSetState(b->rs);
    c->RSSetViewports(b->nvp, b->vp);
    c->RSSetScissorRects(b->nsc, b->sc);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b->rtv, b->dsv);
    c->OMSetBlendState(b->blend, b->blend_factor, b->sample_mask);
    c->OMSetDepthStencilState(b->dss, b->stencil_ref);
    c->VSSetShader(b->vs, nullptr, 0);
    c->PSSetShader(b->ps, nullptr, 0);
    c->VSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, b->vs_cb);
    c->PSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, b->ps_cb);
    c->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, b->ps_srv);
    c->PSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, b->ps_samp);
    c->CSSetShader(b->cs, nullptr, 0);
    c->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, b->cs_cb);
    c->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, b->cs_srv);
    UINT initial[D3D11_1_UAV_SLOT_COUNT] = {0};
    c->CSSetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, b->cs_uav, initial);
    c->CSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, b->cs_samp);

    rel(b->ia_layout); rel(b->ia_index); rel_arr(b->ia_vb);
    rel(b->rs); rel_arr(b->rtv); rel(b->dsv); rel(b->blend); rel(b->dss);
    rel(b->vs); rel(b->ps); rel_arr(b->vs_cb); rel_arr(b->ps_cb);
    rel_arr(b->ps_srv); rel_arr(b->ps_samp);
    rel(b->cs); rel_arr(b->cs_cb); rel_arr(b->cs_srv); rel_arr(b->cs_uav); rel_arr(b->cs_samp);
}
