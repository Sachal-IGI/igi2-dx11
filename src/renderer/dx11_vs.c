/* modules/renderer: Direct3D 8 vertex shaders (vs.1.0 / vs.1.1) to HLSL vs_4_0, after the DirectX 8 SDK reference.
   Every instruction becomes "t = <float4 result>; dst.mask = t.mask;": all sources are read before the write, and the
   components outside the write mask keep their value. Scalar instructions (rcp rsq exp log expp logp) read the fourth
   selector of the source swizzle, the SDK's "source.w": the replicated component for src.x, w for a source without
   swizzle. The address register is an int4; mov a0 floors (vs.1.x), relative constants are c[clamp(a0.x + n, 0, 255)]. */
#include "dx11_vs.h"
#include <d3d8types.h>
#include <string.h>

#define VS_MAX_DECL_TOKENS 4096u
#define VS_MAX_CODE_TOKENS 65536u
#define VS_TEMPS 12u
#define VS_INPUTS 16u
#define VS_CONSTS 256u
#define OP_MOVA 46u          /* D3D9 vs_2_0 only; accepted, rounds to nearest */
#define SP_RELATIVE 0x2000u  /* D3DVS_ADDRMODE_RELATIVE on a source: c[a0.x + n] */

#define P_TYPE(t) (((t) >> 28) & 7u)
#define P_NUM(t) ((unsigned)((t) & 0x7FFu))
#define P_MASK(t) ((unsigned)(((t) >> 16) & 0xFu))
#define P_SWIZZLE(t) ((unsigned)(((t) >> 16) & 0xFFu))
#define P_SRCMOD(t) ((unsigned)(((t) >> 24) & 0xFu))
#define P_DSTMOD(t) ((unsigned)(((t) >> 20) & 0xFu))
#define P_SHIFT(t) ((unsigned)(((t) >> 24) & 0xFu))

enum { RT_TEMP, RT_INPUT, RT_CONST, RT_ADDR, RT_RAST, RT_ATTR, RT_TEXCRD };
enum { H_RCP, H_RSQ, H_LOG, H_LIT, H_DST, H_FRC, H_EXPP, H_LOGP, H_NONE };

typedef struct OpDef { const char *name; BYTE params; BYTE helper; } OpDef; /* params: dest + sources */

static const OpDef kOps[25] = {
    {"nop", 0, H_NONE},  {"mov", 2, H_NONE},  {"add", 3, H_NONE},  {"sub", 3, H_NONE},  {"mad", 4, H_NONE},
    {"mul", 3, H_NONE},  {"rcp", 2, H_RCP},   {"rsq", 2, H_RSQ},   {"dp3", 3, H_NONE},  {"dp4", 3, H_NONE},
    {"min", 3, H_NONE},  {"max", 3, H_NONE},  {"slt", 3, H_NONE},  {"sge", 3, H_NONE},  {"exp", 2, H_NONE},
    {"log", 2, H_LOG},   {"lit", 2, H_LIT},   {"dst", 3, H_DST},   {"lrp", 4, H_NONE},  {"frc", 2, H_FRC},
    {"m4x4", 3, H_NONE}, {"m4x3", 3, H_NONE}, {"m3x4", 3, H_NONE}, {"m3x3", 3, H_NONE}, {"m3x2", 3, H_NONE}};
static const OpDef kMova = {"mova", 2, H_NONE}, kExpp = {"expp", 2, H_EXPP}, kLogp = {"logp", 2, H_LOGP};

static const char *const kHelpers[H_NONE] = {
    /* rcp: 1/x, exactly 1 for 1, +inf for 0 */
    "float nx_rcp(float x) { return x == 1.0 ? 1.0 : (x == 0.0 ? asfloat(0x7F800000u) : 1.0 / x); }\n",
    /* rsq: 1/sqrt(|x|), exactly 1 for 1, +inf for 0 */
    "float nx_rsq(float x) { x = abs(x); return x == 1.0 ? 1.0 : (x == 0.0 ? asfloat(0x7F800000u) : rsqrt(x)); }\n",
    /* log: log2(|x|), -FLT_MAX for 0 */
    "float nx_log(float x) { x = abs(x); return x == 0.0 ? asfloat(0xFF7FFFFFu) : log2(x); }\n",
    "float4 nx_lit(float4 s)\n{\n    float4 d = float4(1.0, 0.0, 0.0, 1.0);\n    if (s.x > 0.0) {\n        d.y = s.x;\n"
    "        if (s.y > 0.0) d.z = exp2(clamp(s.w, -127.9961, 127.9961) * log2(s.y));\n    }\n    return d;\n}\n",
    "float4 nx_dst(float4 a, float4 b) { return float4(1.0, a.y * b.y, a.z, b.w); }\n",
    "float4 nx_frc(float4 x) { return x - floor(x); }\n",
    "float4 nx_expp(float x) { float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1.0); }\n",
    /* logp: exponent and mantissa from the bits as the SDK does, -FLT_MAX for 0 */
    "float4 nx_logp(float x)\n{\n    uint b;\n    x = abs(x);\n"
    "    if (x == 0.0) return float4(asfloat(0xFF7FFFFFu), 1.0, asfloat(0xFF7FFFFFu), 1.0);\n    b = asuint(x);\n"
    "    return float4((float)((int)(b >> 23) - 127), asfloat((b & 0x7FFFFFu) | 0x3F800000u), log2(x), 1.0);\n}\n"};

static const char kHead[] =
    "cbuffer NxVSConstants : register(b1) { float4 c[256]; };\n"
    "cbuffer NxVSFix : register(b2) { float4 nx_fix; };\n";
static const char kOutStruct[] =
    "struct NxVSOut {\n"
    "    float4 pos : SV_Position;\n"
    "    float4 d0 : COLOR0;\n"
    "    float4 d1 : COLOR1;\n"
    "    float4 t0 : TEXCOORD0;\n"
    "    float4 t1 : TEXCOORD1;\n"
    "    float4 t2 : TEXCOORD2;\n"
    "    float4 t3 : TEXCOORD3;\n"
    "    float4 t4 : TEXCOORD4;\n"
    "    float4 t5 : TEXCOORD5;\n"
    "    float4 t6 : TEXCOORD6;\n"
    "    float4 t7 : TEXCOORD7;\n"
    "    float fog : FOG;\n"
    "};\n";
static const char kLocals[] =
    "    float4 oPos = 0.0, oD0 = 1.0, oD1 = 0.0, oFog = 1.0, oPts = 0.0;\n"
    "    float4 oT0 = 0.0, oT1 = 0.0, oT2 = 0.0, oT3 = 0.0, oT4 = 0.0, oT5 = 0.0, oT6 = 0.0, oT7 = 0.0;\n"
    "    float4 t = 0.0;\n";
static const char kTail[] =
    "    NxVSOut o;\n"
    "    o.pos = oPos;\n"
    "    o.d0 = saturate(oD0);\n"
    "    o.d1 = saturate(oD1);\n"
    "    o.t0 = oT0;\n"
    "    o.t1 = oT1;\n"
    "    o.t2 = oT2;\n"
    "    o.t3 = oT3;\n"
    "    o.t4 = oT4;\n"
    "    o.t5 = oT5;\n"
    "    o.t6 = oT6;\n"
    "    o.t7 = oT7;\n"
    "    o.fog = oFog.x;\n"
    "    o.pos.xy += nx_fix.xy * o.pos.w;\n"
    "    return o;\n"
    "}\n";

/* ---- declaration ---- */

static unsigned TypeSize(unsigned type)
{
    switch (type) {
    case D3DVSDT_FLOAT1: return 4;
    case D3DVSDT_FLOAT2: return 8;
    case D3DVSDT_FLOAT3: return 12;
    case D3DVSDT_FLOAT4: return 16;
    case D3DVSDT_D3DCOLOR: return 4;
    case D3DVSDT_UBYTE4: return 4;
    case D3DVSDT_SHORT2: return 4;
    case D3DVSDT_SHORT4: return 8;
    }
    return 0;
}

DXGI_FORMAT NxVS_Format(BYTE d3dvsdt_type)
{
    switch (d3dvsdt_type) {
    case D3DVSDT_FLOAT1: return DXGI_FORMAT_R32_FLOAT;
    case D3DVSDT_FLOAT2: return DXGI_FORMAT_R32G32_FLOAT;
    case D3DVSDT_FLOAT3: return DXGI_FORMAT_R32G32B32_FLOAT;
    case D3DVSDT_FLOAT4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case D3DVSDT_D3DCOLOR: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DVSDT_UBYTE4: return DXGI_FORMAT_R8G8B8A8_UINT;
    case D3DVSDT_SHORT2: return DXGI_FORMAT_R16G16_SINT;
    case D3DVSDT_SHORT4: return DXGI_FORMAT_R16G16B16A16_SINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

static int AddConst(NxVSDecl *d, unsigned reg, const DWORD *v)
{
    unsigned k;
    for (k = 0; k < d->consts; k++)
        if (d->c[k].reg == reg) break;
    if (k == d->consts) {
        if (k >= sizeof d->c / sizeof d->c[0]) return 0;
        d->consts++;
        d->c[k].reg = reg;
    }
    memcpy(d->c[k].v, v, sizeof d->c[k].v);
    return 1;
}

int NxVS_ParseDeclaration(const DWORD *decl, NxVSDecl *out)
{
    unsigned i = 0, stream = 16, seen = 0, k;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!decl) return 0;
    while (i < VS_MAX_DECL_TOKENS) {
        DWORD t = decl[i++];
        switch (t >> D3DVSD_TOKENTYPESHIFT) {
        case D3DVSD_TOKEN_NOP:
            break;
        case D3DVSD_TOKEN_STREAM:
            if (t & D3DVSD_STREAMTESSMASK) return 0; /* tessellator stream */
            stream = (unsigned)(t & D3DVSD_STREAMNUMBERMASK);
            break;
        case D3DVSD_TOKEN_STREAMDATA:
            if (stream > 15) return 0; /* data before any stream token */
            if (t & D3DVSD_DATALOADTYPEMASK) {
                out->stride[stream] += 4u * (unsigned)((t & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT);
            } else {
                unsigned reg = (unsigned)(t & D3DVSD_VERTEXREGMASK);
                unsigned type = (unsigned)((t & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
                unsigned size = TypeSize(type);
                NxVSElement *e;
                if (!size || reg >= VS_INPUTS || ((seen >> reg) & 1u) || out->count >= 16 || out->stride[stream] > 0xFFFFu)
                    return 0;
                seen |= 1u << reg;
                e = &out->e[out->count++];
                e->reg = (BYTE)reg;
                e->stream = (BYTE)stream;
                e->type = (BYTE)type;
                e->pad = 0;
                e->offset = (WORD)out->stride[stream];
                out->stride[stream] += size;
            }
            break;
        case D3DVSD_TOKEN_CONSTMEM: {
            unsigned addr = (unsigned)(t & D3DVSD_CONSTADDRESSMASK);
            unsigned n = (unsigned)((t & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT);
            if (i + 4u * n > VS_MAX_DECL_TOKENS) return 0;
            for (k = 0; k < n; k++, i += 4)
                if (!AddConst(out, addr + k, decl + i)) return 0;
            break;
        }
        case D3DVSD_TOKEN_EXT:
            i += (unsigned)((t & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT);
            break;
        case D3DVSD_TOKEN_END:
            return t == D3DVSD_END();
        default: /* tessellator and unknown tokens */
            return 0;
        }
    }
    return 0;
}

/* ---- function ---- */

typedef struct Out { char *p; size_t len, cap; int bad; } Out;
typedef struct Info { unsigned temps, inputs, helpers; int a0; } Info;

static void Put(Out *o, const char *s)
{
    for (; *s; s++) {
        if (o->len + 1 >= o->cap) { o->bad = 1; return; }
        o->p[o->len++] = *s;
    }
}

static void PutChar(Out *o, char c)
{
    char s[2];
    s[0] = c;
    s[1] = 0;
    Put(o, s);
}

static void PutU(Out *o, unsigned v)
{
    char b[12];
    int n = 11;
    b[11] = 0;
    do {
        b[--n] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && n > 0);
    Put(o, b + n);
}

static const OpDef *FindOp(DWORD op)
{
    if (op < sizeof kOps / sizeof kOps[0]) return &kOps[op];
    if (op == OP_MOVA) return &kMova;
    if (op == D3DSIO_EXPP) return &kExpp;
    if (op == D3DSIO_LOGP) return &kLogp;
    return NULL;
}

static unsigned MatrixRows(DWORD op) /* rows read from src1, src1+1, ... */
{
    switch (op) {
    case D3DSIO_M4x4: return 4;
    case D3DSIO_M4x3: return 3;
    case D3DSIO_M3x4: return 4;
    case D3DSIO_M3x3: return 3;
    case D3DSIO_M3x2: return 2;
    }
    return 0;
}

static int CheckDst(DWORD t, Info *inf)
{
    unsigned num = P_NUM(t);
    if (!(t & 0x80000000u) || (t & SP_RELATIVE) || P_DSTMOD(t) > 1 || P_SHIFT(t)) return 0;
    switch (P_TYPE(t)) {
    case RT_TEMP:
        if (num >= VS_TEMPS) return 0;
        inf->temps |= 1u << num;
        return 1;
    case RT_ADDR:
        if (num) return 0;
        inf->a0 = 1;
        return 1;
    case RT_RAST: return num <= 2;   /* oPos oFog oPts */
    case RT_ATTR: return num <= 1;   /* oD0 oD1 */
    case RT_TEXCRD: return num <= 7; /* oT0..oT7 */
    }
    return 0; /* inputs and constants are read-only */
}

static int CheckSrc(DWORD t, unsigned rows, Info *inf)
{
    unsigned num = P_NUM(t), last = num + rows - 1, k;
    if (!(t & 0x80000000u) || P_SRCMOD(t) > 1) return 0; /* vs.1.x has only negate */
    if (t & SP_RELATIVE) {
        if (P_TYPE(t) != RT_CONST) return 0;
        inf->a0 = 1;
    }
    switch (P_TYPE(t)) {
    case RT_TEMP:
        if (last >= VS_TEMPS) return 0;
        for (k = num; k <= last; k++) inf->temps |= 1u << k;
        return 1;
    case RT_INPUT:
        if (last >= VS_INPUTS) return 0;
        for (k = num; k <= last; k++) inf->inputs |= 1u << k;
        return 1;
    case RT_CONST: return (t & SP_RELATIVE) || last < VS_CONSTS;
    case RT_ADDR:
        if (num || rows != 1) return 0;
        inf->a0 = 1;
        return 1;
    case RT_RAST: return rows == 1 && num <= 2;
    case RT_ATTR: return rows == 1 && num <= 1;
    case RT_TEXCRD: return rows == 1 && num <= 7;
    }
    return 0;
}

/* register name; as_asm: the D3D8 assembler spelling for the comments */
static void PutReg(Out *o, DWORD t, unsigned add, int as_asm)
{
    unsigned num = P_NUM(t) + add;
    switch (P_TYPE(t)) {
    case RT_TEMP: Put(o, "r"); PutU(o, num); break;
    case RT_INPUT: Put(o, "v"); PutU(o, num); break;
    case RT_CONST:
        if (t & SP_RELATIVE) {
            Put(o, as_asm ? "c[a0.x+" : "c[clamp(a0.x + ");
            PutU(o, num);
            Put(o, as_asm ? "]" : ", 0, 255)]");
        } else {
            Put(o, as_asm ? "c" : "c[");
            PutU(o, num);
            if (!as_asm) Put(o, "]");
        }
        break;
    case RT_ADDR: Put(o, "a0"); break;
    case RT_RAST: Put(o, num == 0 ? "oPos" : num == 1 ? "oFog" : "oPts"); break;
    case RT_ATTR: Put(o, "oD"); PutU(o, num); break;
    default: Put(o, "oT"); PutU(o, num); break;
    }
}

static void PutMask(Out *o, unsigned mask)
{
    unsigned k;
    if (mask == 0xF) return;
    Put(o, ".");
    for (k = 0; k < 4; k++)
        if (mask & (1u << k)) PutChar(o, "xyzw"[k]);
}

/* source as HLSL: n = 4 all four selectors, 3 the first three, 1 the fourth only (scalar instructions) */
static void PutSrc(Out *o, DWORD t, unsigned add, unsigned n)
{
    unsigned sw = P_SWIZZLE(t), k;
    Put(o, P_SRCMOD(t) == 1 ? "(-" : "(");
    if (P_TYPE(t) == RT_ADDR) Put(o, "((float4)a0)");
    else PutReg(o, t, add, 0);
    Put(o, ".");
    if (n == 1) PutChar(o, "xyzw"[(sw >> 6) & 3u]);
    else for (k = 0; k < n; k++) PutChar(o, "xyzw"[(sw >> (2 * k)) & 3u]);
    Put(o, ")");
}

static void PutAsm(Out *o, const OpDef *d, const DWORD *p)
{
    unsigned k, i, sw;
    Put(o, "    // ");
    Put(o, d->name);
    if (P_DSTMOD(p[0])) Put(o, "_sat");
    Put(o, " ");
    PutReg(o, p[0], 0, 1);
    PutMask(o, P_MASK(p[0]));
    for (k = 1; k < d->params; k++) {
        Put(o, ", ");
        if (P_SRCMOD(p[k]) == 1) Put(o, "-");
        PutReg(o, p[k], 0, 1);
        sw = P_SWIZZLE(p[k]);
        if (sw == 0xE4) continue;
        Put(o, ".");
        if (sw == 0x00 || sw == 0x55 || sw == 0xAA || sw == 0xFF) PutChar(o, "xyzw"[sw & 3u]);
        else for (i = 0; i < 4; i++) PutChar(o, "xyzw"[(sw >> (2 * i)) & 3u]);
    }
    Put(o, "\n");
}

static void PutCall(Out *o, const char *fn, const DWORD *p, unsigned srcs, unsigned n)
{
    unsigned k;
    Put(o, fn);
    Put(o, "(");
    for (k = 1; k <= srcs; k++) {
        if (k > 1) Put(o, ", ");
        PutSrc(o, p[k], 0, n);
    }
    Put(o, ")");
}

static void Emit(Out *o, DWORD op, const OpDef *d, const DWORD *p)
{
    DWORD dst = p[0];
    unsigned mask = P_MASK(dst), rows = MatrixRows(op), k;
    PutAsm(o, d, p);
    Put(o, "    t = ");
    switch (op) {
    case D3DSIO_MOV: case OP_MOVA: PutSrc(o, p[1], 0, 4); break;
    case D3DSIO_ADD: PutSrc(o, p[1], 0, 4); Put(o, " + "); PutSrc(o, p[2], 0, 4); break;
    case D3DSIO_SUB: PutSrc(o, p[1], 0, 4); Put(o, " - "); PutSrc(o, p[2], 0, 4); break;
    case D3DSIO_MUL: PutSrc(o, p[1], 0, 4); Put(o, " * "); PutSrc(o, p[2], 0, 4); break;
    case D3DSIO_MAD:
        PutSrc(o, p[1], 0, 4); Put(o, " * "); PutSrc(o, p[2], 0, 4); Put(o, " + "); PutSrc(o, p[3], 0, 4);
        break;
    case D3DSIO_LRP: /* src0 * (src1 - src2) + src2 */
        PutSrc(o, p[1], 0, 4); Put(o, " * ("); PutSrc(o, p[2], 0, 4); Put(o, " - "); PutSrc(o, p[3], 0, 4);
        Put(o, ") + "); PutSrc(o, p[3], 0, 4);
        break;
    case D3DSIO_RCP: PutCall(o, "nx_rcp", p, 1, 1); break;
    case D3DSIO_RSQ: PutCall(o, "nx_rsq", p, 1, 1); break;
    case D3DSIO_EXP: PutCall(o, "exp2", p, 1, 1); break;
    case D3DSIO_LOG: PutCall(o, "nx_log", p, 1, 1); break;
    case D3DSIO_EXPP: PutCall(o, "nx_expp", p, 1, 1); break;
    case D3DSIO_LOGP: PutCall(o, "nx_logp", p, 1, 1); break;
    case D3DSIO_DP3: PutCall(o, "dot", p, 2, 3); break;
    case D3DSIO_DP4: PutCall(o, "dot", p, 2, 4); break;
    case D3DSIO_MIN: PutCall(o, "min", p, 2, 4); break;
    case D3DSIO_MAX: PutCall(o, "max", p, 2, 4); break;
    case D3DSIO_SLT: Put(o, "(float4)("); PutSrc(o, p[1], 0, 4); Put(o, " < "); PutSrc(o, p[2], 0, 4); Put(o, ")"); break;
    case D3DSIO_SGE: Put(o, "(float4)("); PutSrc(o, p[1], 0, 4); Put(o, " >= "); PutSrc(o, p[2], 0, 4); Put(o, ")"); break;
    case D3DSIO_LIT: PutCall(o, "nx_lit", p, 1, 4); break;
    case D3DSIO_DST: PutCall(o, "nx_dst", p, 2, 4); break;
    case D3DSIO_FRC: PutCall(o, "nx_frc", p, 1, 4); break;
    default: { /* m4x4 m4x3 m3x4 m3x3 m3x2: dot(src0, src1 + row) for each row, the rest 0 and not written */
        unsigned n = (op == D3DSIO_M4x4 || op == D3DSIO_M4x3) ? 4 : 3;
        Put(o, "float4(");
        for (k = 0; k < 4; k++) {
            if (k) Put(o, ", ");
            if (k >= rows) { Put(o, "0.0"); continue; }
            Put(o, "dot(");
            PutSrc(o, p[1], 0, n);
            Put(o, ", ");
            PutSrc(o, p[2], k, n);
            Put(o, ")");
        }
        Put(o, ")");
        mask &= (1u << rows) - 1u;
        break;
    }
    }
    Put(o, ";\n");
    if (!mask) return;
    Put(o, "    ");
    PutReg(o, dst, 0, 0);
    PutMask(o, mask);
    Put(o, " = ");
    if (P_TYPE(dst) == RT_ADDR)
        Put(o, op == OP_MOVA ? "((int4)clamp(floor(t + 0.5), -1024.0, 1024.0))" : "((int4)clamp(floor(t), -1024.0, 1024.0))");
    else
        Put(o, P_DSTMOD(dst) ? "saturate(t)" : "t");
    PutMask(o, mask);
    Put(o, ";\n");
}

/* o NULL: checks every instruction and fills inf; else writes the HLSL body */
static int Walk(const DWORD *code, Info *inf, Out *o)
{
    unsigned i = 1, k;
    for (;;) {
        DWORD t, op;
        const OpDef *d;
        if (i >= VS_MAX_CODE_TOKENS) return 0;
        t = code[i];
        op = t & 0xFFFFu;
        if (op == D3DSIO_END) return 1;
        if (op == D3DSIO_COMMENT) {
            i += 1u + (unsigned)((t >> 16) & 0x7FFFu);
            continue;
        }
        if (t & 0x80000000u) return 0;
        d = FindOp(op);
        if (!d || i + d->params >= VS_MAX_CODE_TOKENS) return 0;
        if (!o) {
            unsigned rows = MatrixRows(op);
            /* one at a time, so a short shader is never read past its first non-parameter token */
            for (k = 0; k < d->params; k++)
                if (!(k == 0 ? CheckDst(code[i + 1], inf) : CheckSrc(code[i + 1 + k], k == 2 && rows ? rows : 1, inf)))
                    return 0;
            if (d->helper != H_NONE) inf->helpers |= 1u << d->helper;
        } else if (d->params) {
            Emit(o, op, d, code + i + 1);
        }
        i += 1u + d->params;
    }
}

static const char *InputType(unsigned type)
{
    switch (type) {
    case D3DVSDT_FLOAT1: case D3DVSDT_FLOAT2: case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT4: case D3DVSDT_D3DCOLOR:
        return "float4";
    case D3DVSDT_UBYTE4: return "uint4";
    case D3DVSDT_SHORT2: return "int2";
    case D3DVSDT_SHORT4: return "int4";
    }
    return NULL;
}

int NxVS_Translate(const DWORD *code, const NxVSDecl *decl, char *hlsl, size_t cap)
{
    Info inf;
    Out o;
    unsigned k, r, seen = 0, count = decl ? decl->count : 0;
    const NxVSElement *by_reg[VS_INPUTS];
    if (!hlsl || !cap) return 0;
    hlsl[0] = 0;
    if (!code || (code[0] & 0xFFFFFF00u) != 0xFFFE0100u || (code[0] & 0xFFu) > 1u || count > 16) return 0;
    memset(&inf, 0, sizeof inf);
    memset(by_reg, 0, sizeof by_reg);
    for (k = 0; k < count; k++) {
        const NxVSElement *e = &decl->e[k];
        if (e->reg >= VS_INPUTS || ((seen >> e->reg) & 1u) || !InputType(e->type)) return 0;
        seen |= 1u << e->reg;
        by_reg[e->reg] = e;
    }
    if (!Walk(code, &inf, NULL)) return 0;

    o.p = hlsl;
    o.len = 0;
    o.cap = cap;
    o.bad = 0;
    Put(&o, "// Direct3D 8 vs.1.");
    PutU(&o, (unsigned)(code[0] & 0xFFu));
    Put(&o, " translated by dx11_vs.c\n");
    Put(&o, kHead);
    Put(&o, "struct NxVSIn {\n");
    for (k = 0; k < count; k++) {
        Put(&o, "    ");
        Put(&o, InputType(decl->e[k].type));
        Put(&o, " v");
        PutU(&o, decl->e[k].reg);
        Put(&o, " : ATTR");
        PutU(&o, decl->e[k].reg);
        Put(&o, ";\n");
    }
    if (!count) Put(&o, "    uint nx_vertex : SV_VertexID;\n");
    Put(&o, "};\n");
    Put(&o, kOutStruct);
    for (k = 0; k < H_NONE; k++)
        if (inf.helpers & (1u << k)) Put(&o, kHelpers[k]);
    Put(&o, "NxVSOut main(NxVSIn i)\n{\n");
    for (r = 0; r < VS_INPUTS; r++) {
        const NxVSElement *e = by_reg[r];
        if (!((inf.inputs >> r) & 1u)) continue;
        Put(&o, "    float4 v");
        PutU(&o, r);
        Put(&o, " = ");
        if (!e) {
            Put(&o, "float4(0.0, 0.0, 0.0, 1.0)"); /* not in the declaration */
        } else if (e->type == D3DVSDT_SHORT2) {
            Put(&o, "float4((float2)i.v"); PutU(&o, r); Put(&o, ", 0.0, 1.0)");
        } else {
            Put(&o, e->type == D3DVSDT_UBYTE4 || e->type == D3DVSDT_SHORT4 ? "(float4)i.v" : "i.v");
            PutU(&o, r);
        }
        Put(&o, ";\n");
    }
    for (r = 0; r < VS_TEMPS; r++) {
        if (!((inf.temps >> r) & 1u)) continue;
        Put(&o, "    float4 r");
        PutU(&o, r);
        Put(&o, " = 0.0;\n");
    }
    if (inf.a0) Put(&o, "    int4 a0 = 0;\n");
    Put(&o, kLocals);
    Walk(code, &inf, &o);
    Put(&o, kTail);
    if (o.bad) {
        hlsl[0] = 0;
        return 0;
    }
    hlsl[o.len] = 0;
    return (int)o.len;
}
