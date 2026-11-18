/* modules/renderer: Direct3D 8 pixel shaders (ps.1.0 - ps.1.3) to HLSL ps_4_0, after the DirectX 8 SDK reference.
   Every instruction becomes "t = <float4 result>; dst.mask = t.mask;" with the result shift and _sat applied to t: all sources
   are read before the write, and the components outside the write mask keep their value. A co-issued pair computes both
   results (t, u) before either is written. Registers are float4 locals with their assembler names: r0 r1 start at 0, v0 v1
   are the clamped colours, t0..t3 start as the interpolated texture coordinates and hold colours once a texture instruction
   writes them, c0..c7 are c[] or the shader's def values. Nothing is clamped except by _sat and the final saturate(r0).
   The texture addressing instructions read the coordinates of their own stage from i.tN (texm3x2/texm3x3 rows, texdp3,
   texcoord) and sample stage N with FetchN(), which follows stg[N].y like the fixed-function pixel shader. */
#include "dx11_ps.h"
#include <d3d8types.h>
#include <string.h>

#define PS_MAX_CODE_TOKENS 65536u
#define PS_TEMPS 2u
#define PS_INPUTS 2u
#define PS_CONSTS 8u
#define PS_TEXTURES 4u
#define SP_RELATIVE 0x2000u /* relative addressing: vertex shaders only */

#define P_TYPE(t) (((t) >> 28) & 7u)
#define P_NUM(t) ((unsigned)((t) & 0x7FFu))
#define P_MASK(t) ((unsigned)(((t) >> 16) & 0xFu))
#define P_SWIZZLE(t) ((unsigned)(((t) >> 16) & 0xFFu))
#define P_SRCMOD(t) ((unsigned)(((t) >> 24) & 0xFu))
#define P_DSTMOD(t) ((unsigned)(((t) >> 20) & 0xFu))
#define P_SHIFT(t) ((unsigned)(((t) >> 24) & 0xFu))

enum { RT_TEMP, RT_INPUT, RT_CONST, RT_TEXTURE };
enum { K_NOP, K_ARITH, K_DEF, K_TEX }; /* K_TEX: texture addressing, the destination is the stage */

typedef struct OpDef { WORD op; BYTE params; BYTE kind; BYTE minor; const char *name; } OpDef; /* params: dest + sources */

static const OpDef kOps[] = {
    {D3DSIO_NOP, 0, K_NOP, 0, "nop"},
    {D3DSIO_MOV, 2, K_ARITH, 0, "mov"},
    {D3DSIO_ADD, 3, K_ARITH, 0, "add"},
    {D3DSIO_SUB, 3, K_ARITH, 0, "sub"},
    {D3DSIO_MAD, 4, K_ARITH, 0, "mad"},
    {D3DSIO_MUL, 3, K_ARITH, 0, "mul"},
    {D3DSIO_DP3, 3, K_ARITH, 0, "dp3"},
    {D3DSIO_DP4, 3, K_ARITH, 2, "dp4"},
    {D3DSIO_LRP, 4, K_ARITH, 0, "lrp"},
    {D3DSIO_CND, 4, K_ARITH, 0, "cnd"},
    {D3DSIO_CMP, 4, K_ARITH, 2, "cmp"},
    {D3DSIO_DEF, 5, K_DEF, 0, "def"}, /* dest + four raw floats */
    {D3DSIO_TEXCOORD, 1, K_TEX, 0, "texcoord"},
    {D3DSIO_TEXKILL, 1, K_TEX, 0, "texkill"},
    {D3DSIO_TEX, 1, K_TEX, 0, "tex"},
    {D3DSIO_TEXREG2AR, 2, K_TEX, 0, "texreg2ar"},
    {D3DSIO_TEXREG2GB, 2, K_TEX, 0, "texreg2gb"},
    {D3DSIO_TEXREG2RGB, 2, K_TEX, 2, "texreg2rgb"},
    {D3DSIO_TEXM3x2PAD, 2, K_TEX, 0, "texm3x2pad"},
    {D3DSIO_TEXM3x2TEX, 2, K_TEX, 0, "texm3x2tex"},
    {D3DSIO_TEXM3x3PAD, 2, K_TEX, 0, "texm3x3pad"},
    {D3DSIO_TEXM3x3TEX, 2, K_TEX, 0, "texm3x3tex"},
    {D3DSIO_TEXM3x3SPEC, 3, K_TEX, 0, "texm3x3spec"},
    {D3DSIO_TEXM3x3VSPEC, 2, K_TEX, 0, "texm3x3vspec"},
    {D3DSIO_TEXM3x3, 2, K_TEX, 2, "texm3x3"},
    {D3DSIO_TEXDP3, 2, K_TEX, 2, "texdp3"},
    {D3DSIO_TEXDP3TEX, 2, K_TEX, 2, "texdp3tex"}};

/* source modifiers D3DSPSM_NONE..D3DSPSM_X2NEG: HLSL around the swizzled register, assembler around the register */
static const char *const kModPre[9] = {"", "-", "", "0.5 - ", "", "1.0 - ", "1.0 - ", "", ""};
static const char *const kModPost[9] = {"", "", " - 0.5", "", " * 2.0 - 1.0", " * 2.0", "", " * 2.0", " * -2.0"};
static const char *const kAsmPre[9] = {"", "-", "", "-", "", "-", "1-", "", "-"};
static const char *const kAsmPost[9] = {"", "", "_bias", "_bias", "_bx2", "_bx2", "", "_x2", "_x2"};
/* result shift (signed 4 bits): x2 x4 x8, d8 d4 d2; NULL = not a ps.1.x shift */
static const char *const kShiftMul[16] = {"", "2.0", "4.0", "8.0", NULL, NULL, NULL, NULL,
                                          NULL, NULL, NULL, NULL, NULL, "0.125", "0.25", "0.5"};
static const char *const kShiftAsm[16] = {"", "_x2", "_x4", "_x8", NULL, NULL, NULL, NULL,
                                          NULL, NULL, NULL, NULL, NULL, "_d8", "_d4", "_d2"};

/* the declarations the device binds; NxPS and NxVSOut are those of the fixed-function pixel shader (dx11_shaders.c) */
static const char kHead[] =
    "cbuffer NxPS : register(b3) {\n"
    "    int4 cop[8];\n"
    "    int4 aop[8];\n"
    "    int4 stg[8];\n"
    "    float4 tfactor;\n"
    "    float4 fogcolor;\n"
    "    float4 alpharef;\n"
    "    int4 pmisc;\n"
    "    float4 fogtable;\n"
    "};\n"
    "cbuffer NxPSConst : register(b5) { float4 c[8]; };\n"
    "Texture2D tx0 : register(t0); Texture2D tx1 : register(t1); Texture2D tx2 : register(t2); Texture2D tx3 : register(t3);\n"
    "TextureCube tc0 : register(t8); TextureCube tc1 : register(t9); TextureCube tc2 : register(t10); TextureCube tc3 : register(t11);\n"
    "SamplerState sm0 : register(s0); SamplerState sm1 : register(s1); SamplerState sm2 : register(s2); SamplerState sm3 : register(s3);\n"
    "struct NxVSOut {\n"
    "    float4 pos : SV_Position;\n"
    "    float4 d0 : COLOR0;\n"
    "    float4 d1 : COLOR1;\n"
    "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
    "    float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7;\n"
    "    float fog : FOG;\n"
    "};\n";
/* projective divide of the tex instruction, as in the fixed-function pixel shader */
static const char kUv[] =
    "float4 Uv(int s, float4 t) {\n"
    "    int n = stg[s].z;\n"
    "    if (n == 2) return float4(t.xy / t.y, 0, 0);\n"
    "    if (n == 3) return float4(t.xy / t.z, 0, 0);\n"
    "    if (n == 4) return float4(t.xyz / t.w, 0);\n"
    "    return t;\n"
    "}\n";
/* sampling stage # ('#' is the stage number), as FETCH() in the fixed-function pixel shader; one return, or fxc warns X4000 */
static const char kFetch[] =
    "float4 Fetch#(float4 t) {\n"
    "    float4 v = float4(1, 1, 1, 1);\n"
    "    if (stg[#].y == 1) v = tx#.Sample(sm#, t.xy);\n"
    "    else if (stg[#].y == 2) v = tc#.Sample(sm#, t.xyz);\n"
    "    return v;\n"
    "}\n";
/* texm3x3spec / texm3x3vspec: the eye vector reflected about the normal, 2 * (N.E / N.N) * N - E */
static const char kReflect[] = "float3 nx_reflect(float3 n, float3 e) { return 2.0 * dot(n, e) / dot(n, n) * n - e; }\n";
/* r0 is the colour; then fog and the alpha test of the fixed-function pixel shader */
static const char kTail[] =
    "    float4 cur = saturate(r0);\n"
    "    if (pmisc.y != 0) {\n"
    "        float f = i.fog;\n"
    "        if (pmisc.w != 0) {\n"
    "            float z = i.pos.w;\n"
    "            if (pmisc.w == 3) f = (fogtable.y - z) / max(fogtable.y - fogtable.x, 1e-20);\n"
    "            else if (pmisc.w == 1) f = exp(-z * fogtable.z);\n"
    "            else f = exp(-(z * fogtable.z) * (z * fogtable.z));\n"
    "        }\n"
    "        cur.rgb = lerp(fogcolor.rgb, cur.rgb, saturate(f));\n"
    "    }\n"
    "    if (pmisc.x != 8) {\n"
    "        float a = floor(saturate(cur.a) * 255 + 0.5), r = alpharef.x;\n"
    "        if (pmisc.x >= 16) {\n"
    "            float c = saturate(cur.a) * 255;\n"
    "            cur.a = saturate((c - r) / max(fwidth(c), 0.0001) + 0.5);\n"
    "        } else {\n"
    "            bool ok = pmisc.x == 2 ? a < r : pmisc.x == 3 ? a == r : pmisc.x == 4 ? a <= r : pmisc.x == 5 ? a > r :\n"
    "                        pmisc.x == 6 ? a != r : pmisc.x == 7 ? a >= r : false;\n"
    "            if (!ok) discard;\n"
    "        }\n"
    "    }\n"
    "    return cur;\n"
    "}\n";

typedef struct Out { char *p; size_t len, cap; int bad; } Out;
typedef struct Info {
    unsigned minor;                         /* ps.1.minor */
    unsigned temps, inputs, consts, texregs; /* registers used, one bit each */
    unsigned fetch, defs;                   /* stages sampled; constants given by def */
    DWORD defv[PS_CONSTS][4];
    int uv, mat, refl;                      /* helpers and locals needed */
} Info;

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

static void PutHex(Out *o, DWORD v)
{
    char b[12];
    unsigned k;
    b[0] = '0';
    b[1] = 'x';
    for (k = 0; k < 8; k++) b[2 + k] = "0123456789ABCDEF"[(v >> (28 - 4 * k)) & 0xFu];
    b[10] = 'u';
    b[11] = 0;
    Put(o, b);
}

/* a def value for the comments: up to six decimals, the bits when it is huge or not a number */
static void PutF(Out *o, DWORD bits)
{
    float f;
    double v;
    unsigned ip, fp, k;
    char d[7];
    memcpy(&f, &bits, sizeof f);
    v = f;
    if (!(v > -1e7 && v < 1e7)) {
        PutHex(o, bits);
        return;
    }
    if (v < 0.0) {
        Put(o, "-");
        v = -v;
    }
    ip = (unsigned)v;
    fp = (unsigned)((v - (double)ip) * 1000000.0 + 0.5);
    if (fp >= 1000000u) {
        ip++;
        fp -= 1000000u;
    }
    PutU(o, ip);
    Put(o, ".");
    for (k = 6; k-- > 0;) {
        d[k] = (char)('0' + fp % 10u);
        fp /= 10u;
    }
    d[6] = 0;
    for (k = 6; k > 1 && d[k - 1] == '0'; k--) d[k - 1] = 0;
    Put(o, d);
}

/* s with every '#' replaced by the digit n */
static void PutWithDigit(Out *o, const char *s, unsigned n)
{
    for (; *s; s++) PutChar(o, *s == '#' ? (char)('0' + n) : *s);
}

static const OpDef *FindOp(DWORD op)
{
    unsigned k;
    for (k = 0; k < sizeof kOps / sizeof kOps[0]; k++)
        if ((DWORD)kOps[k].op == op) return &kOps[k];
    return NULL;
}

static int CheckDst(DWORD t, unsigned kind, Info *inf)
{
    unsigned num = P_NUM(t), type = P_TYPE(t);
    if (!(t & 0x80000000u) || (t & SP_RELATIVE)) return 0;
    if (kind == K_DEF) return type == RT_CONST && num < PS_CONSTS;
    if (!P_MASK(t) || P_DSTMOD(t) > 1 || !kShiftMul[P_SHIFT(t)]) return 0;
    if (type == RT_TEXTURE && num < PS_TEXTURES) {
        inf->texregs |= 1u << num;
        return 1;
    }
    if (type == RT_TEMP && num < PS_TEMPS && kind == K_ARITH) {
        inf->temps |= 1u << num;
        return 1;
    }
    return 0; /* inputs and constants are read-only; texture instructions write their stage's t register */
}

static int CheckSrc(DWORD t, Info *inf)
{
    unsigned num = P_NUM(t);
    if (!(t & 0x80000000u) || (t & SP_RELATIVE) || P_SRCMOD(t) > 8) return 0; /* _dz/_dw are ps.1.4 */
    switch (P_TYPE(t)) {
    case RT_TEMP:
        if (num >= PS_TEMPS) return 0;
        inf->temps |= 1u << num;
        return 1;
    case RT_INPUT:
        if (num >= PS_INPUTS) return 0;
        inf->inputs |= 1u << num;
        return 1;
    case RT_CONST:
        if (num >= PS_CONSTS) return 0;
        inf->consts |= 1u << num;
        return 1;
    case RT_TEXTURE:
        if (num >= PS_TEXTURES) return 0;
        inf->texregs |= 1u << num;
        return 1;
    }
    return 0;
}

/* what an instruction needs besides its registers (check pass) */
static void Note(Info *inf, DWORD op, const DWORD *p)
{
    unsigned n = P_NUM(p[0]);
    switch (op) {
    case D3DSIO_DEF:
        inf->defs |= 1u << n;
        memcpy(inf->defv[n], p + 1, sizeof inf->defv[n]);
        break;
    case D3DSIO_TEX:
        inf->uv = 1;
        inf->fetch |= 1u << n;
        break;
    case D3DSIO_TEXM3x3SPEC: case D3DSIO_TEXM3x3VSPEC:
        inf->refl = 1;
        inf->fetch |= 1u << n;
        break;
    case D3DSIO_TEXREG2AR: case D3DSIO_TEXREG2GB: case D3DSIO_TEXREG2RGB: case D3DSIO_TEXDP3TEX:
    case D3DSIO_TEXM3x2TEX: case D3DSIO_TEXM3x3TEX:
        inf->fetch |= 1u << n;
        break;
    }
    if ((op >= D3DSIO_TEXM3x2PAD && op <= D3DSIO_TEXM3x3VSPEC) || op == D3DSIO_TEXM3x3) inf->mat = 1;
}

/* texm3x2 / texm3x3 sequences: pad rows on consecutive stages, then the instruction that uses them. *mat is 0x21 after the
   texm3x2pad, 0x31 / 0x32 after one / two texm3x3pad, else 0. Returns the nx_m component the instruction computes (0..2),
   3 for an instruction outside a matrix, -1 out of order. */
static int MatRow(DWORD op, unsigned num, unsigned *mat, unsigned *base)
{
    switch (op) {
    case D3DSIO_TEXM3x2PAD:
        if (*mat) return -1;
        *mat = 0x21;
        *base = num;
        return 0;
    case D3DSIO_TEXM3x2TEX:
        if (*mat != 0x21 || num != *base + 1) return -1;
        *mat = 0;
        return 1;
    case D3DSIO_TEXM3x3PAD:
        if (!*mat) {
            *mat = 0x31;
            *base = num;
            return 0;
        }
        if (*mat != 0x31 || num != *base + 1) return -1;
        *mat = 0x32;
        return 1;
    case D3DSIO_TEXM3x3TEX: case D3DSIO_TEXM3x3SPEC: case D3DSIO_TEXM3x3VSPEC: case D3DSIO_TEXM3x3:
        if (*mat != 0x32 || num != *base + 2) return -1;
        *mat = 0;
        return 2;
    }
    return *mat ? -1 : 3;
}

static void PutReg(Out *o, DWORD t)
{
    PutChar(o, "rvct"[P_TYPE(t) & 3u]);
    PutU(o, P_NUM(t));
}

static void PutMask(Out *o, unsigned mask, const char *names)
{
    unsigned k;
    if (mask == 0xF) return;
    Put(o, ".");
    for (k = 0; k < 4; k++)
        if (mask & (1u << k)) PutChar(o, names[k]);
}

/* source as HLSL: n = 4 all four selectors, 3 the first three, 1 the fourth only (the cnd condition, "r0.a") */
static void PutSrc(Out *o, DWORD t, unsigned n)
{
    unsigned sw = P_SWIZZLE(t), mod = P_SRCMOD(t), k;
    Put(o, "(");
    Put(o, kModPre[mod]);
    PutReg(o, t);
    if (n != 4 || sw != 0xE4) {
        Put(o, ".");
        if (n == 1) PutChar(o, "xyzw"[(sw >> 6) & 3u]);
        else for (k = 0; k < n; k++) PutChar(o, "xyzw"[(sw >> (2 * k)) & 3u]);
    }
    Put(o, kModPost[mod]);
    Put(o, ")");
}

static void PutAsm(Out *o, const OpDef *d, const DWORD *p, int coissue)
{
    unsigned k, j, sw, mod;
    Put(o, coissue ? "    // +" : "    // ");
    Put(o, d->name);
    if (d->params) {
        Put(o, kShiftAsm[P_SHIFT(p[0])]);
        if (P_DSTMOD(p[0])) Put(o, "_sat");
        Put(o, " ");
        PutReg(o, p[0]);
        PutMask(o, P_MASK(p[0]), "rgba");
    }
    for (k = 1; k < d->params; k++) {
        sw = P_SWIZZLE(p[k]);
        mod = P_SRCMOD(p[k]);
        Put(o, ", ");
        Put(o, kAsmPre[mod]);
        PutReg(o, p[k]);
        Put(o, kAsmPost[mod]);
        if (sw == 0xE4) continue;
        Put(o, ".");
        if (sw == 0x00 || sw == 0x55 || sw == 0xAA || sw == 0xFF) PutChar(o, "rgba"[sw & 3u]);
        else for (j = 0; j < 4; j++) PutChar(o, "rgba"[(sw >> (2 * j)) & 3u]);
    }
    Put(o, "\n");
}

/* dst.mask = var with the result shift and _sat */
static void PutWrite(Out *o, DWORD dst, const char *var)
{
    unsigned mask = P_MASK(dst), shift = P_SHIFT(dst);
    int sat = P_DSTMOD(dst) != 0, paren = sat || (shift && mask != 0xF);
    Put(o, "    ");
    PutReg(o, dst);
    PutMask(o, mask, "xyzw");
    Put(o, " = ");
    Put(o, sat ? "saturate(" : paren ? "(" : "");
    Put(o, var);
    if (shift) {
        Put(o, " * ");
        Put(o, kShiftMul[shift]);
    }
    if (paren) Put(o, ")");
    PutMask(o, mask, "xyzw");
    Put(o, ";\n");
}

static void PutArith(Out *o, DWORD op, const DWORD *p, const char *var)
{
    Put(o, "    ");
    Put(o, var);
    Put(o, " = ");
    switch (op) {
    case D3DSIO_MOV: PutSrc(o, p[1], 4); break;
    case D3DSIO_ADD: PutSrc(o, p[1], 4); Put(o, " + "); PutSrc(o, p[2], 4); break;
    case D3DSIO_SUB: PutSrc(o, p[1], 4); Put(o, " - "); PutSrc(o, p[2], 4); break;
    case D3DSIO_MUL: PutSrc(o, p[1], 4); Put(o, " * "); PutSrc(o, p[2], 4); break;
    case D3DSIO_MAD:
        PutSrc(o, p[1], 4); Put(o, " * "); PutSrc(o, p[2], 4); Put(o, " + "); PutSrc(o, p[3], 4);
        break;
    case D3DSIO_LRP: /* src0 * (src1 - src2) + src2 */
        PutSrc(o, p[1], 4); Put(o, " * ("); PutSrc(o, p[2], 4); Put(o, " - "); PutSrc(o, p[3], 4);
        Put(o, ") + "); PutSrc(o, p[3], 4);
        break;
    case D3DSIO_DP3: Put(o, "dot("); PutSrc(o, p[1], 3); Put(o, ", "); PutSrc(o, p[2], 3); Put(o, ")"); break;
    case D3DSIO_DP4: Put(o, "dot("); PutSrc(o, p[1], 4); Put(o, ", "); PutSrc(o, p[2], 4); Put(o, ")"); break;
    case D3DSIO_CND: /* ps.1.0-1.3: src0 is r0.a, one condition for all components */
        Put(o, "("); PutSrc(o, p[1], 1); Put(o, " > 0.5) ? "); PutSrc(o, p[2], 4); Put(o, " : "); PutSrc(o, p[3], 4);
        break;
    default: /* cmp: per component */
        Put(o, "("); PutSrc(o, p[1], 4); Put(o, " >= 0.0) ? "); PutSrc(o, p[2], 4); Put(o, " : "); PutSrc(o, p[3], 4);
        break;
    }
    Put(o, ";\n");
}

static void PutFetchCall(Out *o, unsigned n)
{
    Put(o, "Fetch");
    PutU(o, n);
    Put(o, "(");
}

/* texture addressing instruction on stage n = its destination register; row as MatRow, base = first row's stage */
static void PutTex(Out *o, DWORD op, const DWORD *p, int row, unsigned base)
{
    unsigned n = P_NUM(p[0]), k;
    if (op == D3DSIO_TEXKILL) { /* any of the first three coordinates below 0 */
        Put(o, "    clip(t");
        PutU(o, n);
        Put(o, ".xyz);\n");
        return;
    }
    if (row >= 0 && row < 3) { /* one row of the matrix: this stage's coordinates . the source */
        Put(o, "    nx_m.");
        PutChar(o, "xyz"[row]);
        Put(o, " = dot(i.t");
        PutU(o, n);
        Put(o, ".xyz, ");
        PutSrc(o, p[1], 3);
        Put(o, ");\n");
        if (op == D3DSIO_TEXM3x2PAD || op == D3DSIO_TEXM3x3PAD) return;
    }
    Put(o, "    t = ");
    switch (op) {
    case D3DSIO_TEXCOORD:
        Put(o, "float4(saturate(i.t"); PutU(o, n); Put(o, ".xyz), 1.0)");
        break;
    case D3DSIO_TEX:
        PutFetchCall(o, n); Put(o, "Uv("); PutU(o, n); Put(o, ", i.t"); PutU(o, n); Put(o, "))");
        break;
    case D3DSIO_TEXREG2AR:
        PutFetchCall(o, n); Put(o, "float4("); PutSrc(o, p[1], 4); Put(o, ".wx, 0.0, 0.0))");
        break;
    case D3DSIO_TEXREG2GB:
        PutFetchCall(o, n); Put(o, "float4("); PutSrc(o, p[1], 4); Put(o, ".yz, 0.0, 0.0))");
        break;
    case D3DSIO_TEXREG2RGB:
        PutFetchCall(o, n); Put(o, "float4("); PutSrc(o, p[1], 3); Put(o, ", 0.0))");
        break;
    case D3DSIO_TEXDP3TEX:
        PutFetchCall(o, n); Put(o, "float4(dot(i.t"); PutU(o, n); Put(o, ".xyz, "); PutSrc(o, p[1], 3);
        Put(o, "), 0.0, 0.0, 0.0))");
        break;
    case D3DSIO_TEXDP3:
        Put(o, "dot(i.t"); PutU(o, n); Put(o, ".xyz, "); PutSrc(o, p[1], 3); Put(o, ")");
        break;
    case D3DSIO_TEXM3x2TEX:
        PutFetchCall(o, n); Put(o, "float4(nx_m.xy, 0.0, 0.0))");
        break;
    case D3DSIO_TEXM3x3TEX:
        PutFetchCall(o, n); Put(o, "float4(nx_m, 0.0))");
        break;
    case D3DSIO_TEXM3x3:
        Put(o, "float4(nx_m, 1.0)");
        break;
    case D3DSIO_TEXM3x3SPEC: /* eye vector from a constant */
        PutFetchCall(o, n); Put(o, "float4(nx_reflect(nx_m, "); PutSrc(o, p[2], 3); Put(o, "), 0.0))");
        break;
    default: /* texm3x3vspec: eye vector from the w of the three rows' coordinates */
        PutFetchCall(o, n);
        Put(o, "float4(nx_reflect(nx_m, float3(");
        for (k = 0; k < 3; k++) {
            if (k) Put(o, ", ");
            Put(o, "i.t");
            PutU(o, base + k);
            Put(o, ".w");
        }
        Put(o, ")), 0.0))");
        break;
    }
    Put(o, ";\n");
    PutWrite(o, p[0], "t");
}

/* index of the first token at or after i that is not a comment */
static unsigned SkipComments(const DWORD *code, unsigned i)
{
    while (i < PS_MAX_CODE_TOKENS && (code[i] & 0xFFFFu) == D3DSIO_COMMENT) i += 1u + (unsigned)((code[i] >> 16) & 0x7FFFu);
    return i;
}

/* o NULL: checks every instruction and fills inf; else writes the HLSL body */
static int Walk(const DWORD *code, Info *inf, Out *o)
{
    unsigned i = 1, mat = 0, base = 0, k;
    int pair = 0; /* the instruction before can take a co-issued partner */
    for (;;) {
        DWORD t, op;
        const OpDef *d;
        const DWORD *p;
        int row;
        i = SkipComments(code, i);
        if (i >= PS_MAX_CODE_TOKENS) return 0;
        t = code[i];
        op = t & 0xFFFFu;
        if (op == D3DSIO_END) return !mat;
        if (t & 0x80000000u) return 0;
        d = FindOp(op);
        if (!d || d->minor > inf->minor || i + d->params >= PS_MAX_CODE_TOKENS) return 0;
        p = code + i + 1;
        if (!o) {
            if (t & D3DSI_COISSUE) {
                if (d->kind != K_ARITH || !pair) return 0;
                pair = 0;
            } else {
                pair = d->kind == K_ARITH;
            }
            /* one at a time, so a short shader is never read past its first non-parameter token */
            for (k = 0; k < d->params && (k == 0 || d->kind != K_DEF); k++)
                if (!(k == 0 ? CheckDst(p[0], d->kind, inf) : CheckSrc(p[k], inf))) return 0;
        }
        row = MatRow(op, d->params ? P_NUM(p[0]) : 0, &mat, &base);
        if (row < 0) return 0;
        if (!o) {
            if (d->params) Note(inf, op, p);
        } else if (d->kind == K_TEX) {
            PutAsm(o, d, p, 0);
            PutTex(o, op, p, row, base);
        } else if (d->kind == K_ARITH && !(t & D3DSI_COISSUE)) {
            /* the check pass made sure a co-issued instruction follows an arithmetic one */
            unsigned j = SkipComments(code, i + 1u + d->params);
            PutAsm(o, d, p, 0);
            if (j < PS_MAX_CODE_TOKENS && (code[j] & D3DSI_COISSUE)) {
                DWORD op2 = code[j] & 0xFFFFu;
                PutAsm(o, FindOp(op2), code + j + 1, 1);
                PutArith(o, op, p, "t");
                PutArith(o, op2, code + j + 1, "u");
                PutWrite(o, p[0], "t");
                PutWrite(o, code[j + 1], "u");
            } else {
                PutArith(o, op, p, "t");
                PutWrite(o, p[0], "t");
            }
        }
        i += 1u + d->params;
    }
}

int NxPS_Translate(const DWORD *code, char *hlsl, size_t cap)
{
    Info inf;
    Out o;
    unsigned r, k;
    if (!hlsl || !cap) return 0;
    hlsl[0] = 0;
    if (!code || (code[0] & 0xFFFFFF00u) != 0xFFFF0100u || (code[0] & 0xFFu) > 3u) return 0;
    memset(&inf, 0, sizeof inf);
    inf.minor = (unsigned)(code[0] & 0xFFu);
    inf.temps = 1; /* r0 is the result */
    if (!Walk(code, &inf, NULL)) return 0;

    o.p = hlsl;
    o.len = 0;
    o.cap = cap;
    o.bad = 0;
    Put(&o, "// Direct3D 8 ps.1.");
    PutU(&o, inf.minor);
    Put(&o, " translated by dx11_ps.c\n");
    Put(&o, kHead);
    if (inf.uv) Put(&o, kUv);
    for (r = 0; r < PS_TEXTURES; r++)
        if ((inf.fetch >> r) & 1u) PutWithDigit(&o, kFetch, r);
    if (inf.refl) Put(&o, kReflect);
    Put(&o, "float4 main(NxVSOut i) : SV_Target\n{\n");
    for (r = 0; r < PS_INPUTS; r++) {
        if (!((inf.inputs >> r) & 1u)) continue;
        PutWithDigit(&o, "    float4 v# = saturate(i.d#);\n", r);
    }
    for (r = 0; r < PS_TEXTURES; r++) {
        if (!((inf.texregs >> r) & 1u)) continue;
        PutWithDigit(&o, "    float4 t# = i.t#;\n", r);
    }
    for (r = 0; r < PS_CONSTS; r++) {
        if (!((inf.consts >> r) & 1u)) continue;
        if (!((inf.defs >> r) & 1u)) {
            PutWithDigit(&o, "    float4 c# = c[#];\n", r);
            continue;
        }
        PutWithDigit(&o, "    float4 c# = asfloat(uint4(", r);
        for (k = 0; k < 4; k++) {
            if (k) Put(&o, ", ");
            PutHex(&o, inf.defv[r][k]);
        }
        PutWithDigit(&o, ")); // def c#", r);
        for (k = 0; k < 4; k++) {
            Put(&o, ", ");
            PutF(&o, inf.defv[r][k]);
        }
        Put(&o, "\n");
    }
    for (r = 0; r < PS_TEMPS; r++) {
        if (!((inf.temps >> r) & 1u)) continue;
        PutWithDigit(&o, "    float4 r# = 0.0;\n", r);
    }
    if (inf.mat) Put(&o, "    float3 nx_m = 0.0;\n");
    Put(&o, "    float4 t = 0.0, u = 0.0;\n");
    Walk(code, &inf, &o);
    Put(&o, kTail);
    if (o.bad) {
        hlsl[0] = 0;
        return 0;
    }
    hlsl[o.len] = 0;
    return (int)o.len;
}
