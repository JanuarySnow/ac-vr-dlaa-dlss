// SPDX-License-Identifier: GPL-3.0-or-later
// research_stubs — no-op stand-ins for the offline investigation modules.
//
// Frame capture (cap.cpp) and the NGX/CSP observation hooks (ngx_spy.cpp) exist only to
// analyse the renderer offline; they are not part of the shipped mod and live in the
// research repo instead. The call sites remain in the pipeline so a research build can
// drop the real implementations in without touching the rest of the code.
//
// A research build defines ACRE_RESEARCH and compiles the real modules; build.bat picks
// one or the other so their symbols never collide. See build.bat and the research repo's
// dlaa/src.

#ifndef ACRE_RESEARCH

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

extern "C" {

// offline frame capture
void acre_cap_poll(void) {}
void acre_cap_tick(void) {}
int  acre_cap_active(void) { return 0; }
void acre_cap_endframe(void) {}
void acre_cap_in(ID3D11Device *, ID3D11DeviceContext *, int, ID3D11Texture2D *) {}
void acre_cap_out(ID3D11Device *, ID3D11DeviceContext *, int, ID3D11Texture2D *,
                  ID3D11Texture2D *) {}
void acre_cap_eyes(ID3D11Device *, ID3D11DeviceContext *, ID3D11Texture2D *,
                   ID3D11Texture2D *) {}

// NGX / CSP observation
void acre_ngx_spy_install(void) {}
void acre_ngx_spy_start_early(void) {}
void acre_mvhunt_install(ID3D11Device *) {}

}  // extern "C"

#endif  // !ACRE_RESEARCH
