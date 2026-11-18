#ifndef NEONIX_DX11_PS_H
#define NEONIX_DX11_PS_H
/* Direct3D 8 pixel shaders (ps.1.0 - ps.1.3 tokens) to HLSL ps_4_0 source for the Direct3D 11 renderer.
   No state: every call works only on its arguments.

   HLSL contract of NxPS_Translate:
     cbuffer NxPS      : register(b3)  the fixed-function pixel shader's buffer (stg[], fog and alpha test fields are read)
     cbuffer NxPSConst : register(b5) { float4 c[8]; };   SetPixelShaderConstant c0..c7 (a def in the shader replaces its c)
     Texture2D tx0..tx3 : t0..t3, TextureCube tc0..tc3 : t8..t11, SamplerState sm0..sm3 : s0..s3
       stg[n].y = what stage n has bound: 0 none (samples give 1,1,1,1), 1 = 2D (txN), 2 = cube (tcN, sampled with .xyz);
       stg[n].z = projective divide of the tex instruction, as Uv() in the fixed-function pixel shader
     struct NxVSOut exactly as the vertex shaders write it; float4 main(NxVSOut i) : SV_Target
   v0 v1 = saturate(i.d0), saturate(i.d1); t0..t3 start as i.t0..i.t3. The result is saturate(r0), then fog and the alpha
   test as in the fixed-function pixel shader (no specular add: D3D8 has none with pixel shaders).
   Not supported (0): ps.1.4 and later, texbem/texbeml (need the bump matrix), texm3x2depth (writes depth), texm3x3diff. */
#include <windows.h>
#include <stddef.h>

/* HLSL text for the function tokens (version token first, D3DSIO_END last); returns the length without the 0, or 0 when the
   shader is not ps.1.0-1.3, uses an unsupported opcode/register/modifier, has a texm3x2/texm3x3 sequence out of order, or
   the text does not fit in cap (hlsl is then ""). 16 KB holds any valid ps.1.x shader. */
int NxPS_Translate(const DWORD *code, char *hlsl, size_t cap);

#endif
