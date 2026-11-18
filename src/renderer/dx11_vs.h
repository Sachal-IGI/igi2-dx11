#ifndef NEONIX_DX11_VS_H
#define NEONIX_DX11_VS_H
/* Direct3D 8 vertex shaders (vs.1.0 / vs.1.1 tokens + D3DVSD declaration) to HLSL vs_4_0 source for the Direct3D 11 renderer.
   No state: every call works only on its arguments.

   HLSL contract of NxVS_Translate:
     cbuffer NxVSConstants : register(b1) { float4 c[256]; };   the D3D8 constants c0..c255
     cbuffer NxVSFix       : register(b2) { float4 nx_fix; };   xy * oPos.w is added to oPos.xy at the end (half-pixel fix)
     struct NxVSIn  { one member per declared input: float4 vN : ATTRn (uint4 for UBYTE4, int2 for SHORT2, int4 for SHORT4) };
                    (a declaration without inputs gives one "uint nx_vertex : SV_VertexID" instead)
     struct NxVSOut { float4 pos : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;
                      float4 t0..t7 : TEXCOORD0..7; float fog : FOG; };
     NxVSOut main(NxVSIn i)
   Unwritten outputs: pos 0, d0 1, d1 0, t0..t7 0, fog 1. d0/d1 are clamped to 0..1 (D3D8 clamps oD0/oD1 per vertex).
   A v register the shader reads but the declaration lacks reads (0,0,0,1). */
#include <windows.h>
#include <dxgiformat.h>
#include <stddef.h>

typedef struct NxVSElement {
    BYTE reg;     /* v register 0..15 */
    BYTE stream;  /* 0..15 */
    BYTE type;    /* D3DVSDT_* */
    BYTE pad;
    WORD offset;  /* bytes from the start of the vertex in its stream */
} NxVSElement;

typedef struct NxVSConst {
    unsigned reg;  /* c register */
    float v[4];
} NxVSConst;

typedef struct NxVSDecl {
    unsigned count;
    NxVSElement e[16];   /* in declaration order */
    unsigned stride[16]; /* bytes per vertex of each stream (registers + D3DVSD_SKIP), 0 for unused streams */
    unsigned consts;
    NxVSConst c[96];     /* D3DVSD_CONST data, one entry per register (a later value for the same register replaces it);
                            the device loads these into c[] when the shader is set */
} NxVSDecl;

/* 1 = ok; 0 = bad or unsupported (tessellator, more than 16 inputs, register twice, data before a stream token...). */
int NxVS_ParseDeclaration(const DWORD *decl, NxVSDecl *out);
/* HLSL text for the function tokens (version token first, D3DSIO_END last); returns the length without the 0, or 0 when the
   shader is not vs.1.0/1.1, uses an unsupported opcode/register, or the text does not fit in cap (hlsl is then ""). decl may
   be NULL (no inputs). The game's shaders need 1.5-4.5 KB; 64 KB holds any 128-instruction vs.1.1 shader. */
int NxVS_Translate(const DWORD *code, const NxVSDecl *decl, char *hlsl, size_t cap);
/* Input layout format for a D3DVSDT_* type; DXGI_FORMAT_UNKNOWN for an unknown type. */
DXGI_FORMAT NxVS_Format(BYTE d3dvsdt_type);

#endif
