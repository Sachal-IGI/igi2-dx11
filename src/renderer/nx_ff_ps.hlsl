// Neonix renderer (Direct3D 11): Direct3D 8's texture stages, specular add, fog and alpha test, from the constant buffer.
// Arguments are D3DTA values (selector | complement 0x10 | alpha replicate 0x20); ops are D3DTOP values.
// Compiled ahead of time by tools/build_renderer_shaders.py into dx11_shader_code.h.
#include "nx_common.hlsli"

cbuffer NxPS : register(b3) {
    int4 cop[8];       // colour: op, arg1, arg2, arg0
    int4 aop[8];       // alpha: op, arg1, arg2, arg0
    int4 stg[8];       // result arg, texture kind (0 none, 1 2D, 2 cube), projective divide (component count), -
    float4 tfactor;
    float4 fogcolor;
    float4 alpharef;   // x: 0..255
    int4 pmisc;        // alpha func (D3DCMP; 8 always), fog on, specular add, table fog mode
    float4 fogtable;   // start, end, density
};
Texture2D tx0 : register(t0); Texture2D tx1 : register(t1); Texture2D tx2 : register(t2); Texture2D tx3 : register(t3);
Texture2D tx4 : register(t4); Texture2D tx5 : register(t5); Texture2D tx6 : register(t6); Texture2D tx7 : register(t7);
TextureCube tc0 : register(t8); TextureCube tc1 : register(t9); TextureCube tc2 : register(t10); TextureCube tc3 : register(t11);
TextureCube tc4 : register(t12); TextureCube tc5 : register(t13); TextureCube tc6 : register(t14); TextureCube tc7 : register(t15);
SamplerState sm0 : register(s0); SamplerState sm1 : register(s1); SamplerState sm2 : register(s2); SamplerState sm3 : register(s3);
SamplerState sm4 : register(s4); SamplerState sm5 : register(s5); SamplerState sm6 : register(s6); SamplerState sm7 : register(s7);

float4 Arg(int a, float4 cur, float4 dif, float4 spc, float4 tex, float4 tmp)
{
    float4 v;
    int sel = a & 15;
    if (sel == 0) v = dif;
    else if (sel == 1) v = cur;
    else if (sel == 2) v = tex;
    else if (sel == 3) v = tfactor;
    else if (sel == 4) v = spc;
    else v = tmp;
    if (a & 0x10) v = 1 - v;
    if (a & 0x20) v = v.aaaa;
    return v;
}

float4 Op(int op, float4 a1, float4 a2, float4 a0, float4 cur, float4 dif, float4 tex, bool color)
{
    if (op == 2) return a1;
    if (op == 3) return a2;
    if (op == 4) return a1 * a2;
    if (op == 5) return saturate(a1 * a2 * 2);
    if (op == 6) return saturate(a1 * a2 * 4);
    if (op == 7) return saturate(a1 + a2);
    if (op == 8) return saturate(a1 + a2 - 0.5);
    if (op == 9) return saturate((a1 + a2 - 0.5) * 2);
    if (op == 10) return saturate(a1 - a2);
    if (op == 11) return saturate(a1 + a2 - a1 * a2);
    if (op == 12) return lerp(a2, a1, dif.a);
    if (op == 13) return lerp(a2, a1, tex.a);
    if (op == 14) return lerp(a2, a1, tfactor.a);
    if (op == 15) return saturate(a1 + a2 * (1 - tex.a));
    if (op == 16) return lerp(a2, a1, cur.a);
    if (op == 17) return a1;
    if (op == 18) return color ? saturate(a1 + a1.a * a2) : a1;
    if (op == 19) return color ? a1 * a2 : saturate(a1 + a2);
    if (op == 20) return color ? saturate(a1 * (1 - a1.a) + a2) : a1;
    if (op == 21) return color ? saturate((1 - a1) * a2 + a1.a) : saturate(a1 + a2);
    if (op == 24) return saturate(dot(a1.rgb - 0.5, a2.rgb - 0.5) * 4).xxxx;
    if (op == 25) return saturate(a0 + a1 * a2);
    if (op == 26) return lerp(a2, a1, a0);
    return cur;
}

float4 Uv(int s, float4 t)
{
    int n = stg[s].z;
    if (n == 2) return float4(t.xy / t.y, 0, 0);
    if (n == 3) return float4(t.xy / t.z, 0, 0);
    if (n == 4) return float4(t.xyz / t.w, 0);
    return t;
}

#define FETCH(n) \
    if (stg[n].y == 1) tex[n] = tx##n.Sample(sm##n, Uv(n, i.t##n).xy); \
    else if (stg[n].y == 2) tex[n] = tc##n.Sample(sm##n, Uv(n, i.t##n).xyz); \
    else tex[n] = float4(1, 1, 1, 1);

float4 main(NxVSOut i) : SV_Target
{
    float4 tex[8];
    FETCH(0) FETCH(1) FETCH(2) FETCH(3) FETCH(4) FETCH(5) FETCH(6) FETCH(7)
    float4 dif = i.d0, spc = i.d1, cur = i.d0, tmp = float4(0, 0, 0, 0);
    [unroll] for (int s = 0; s < 8; s++) {
        if (cop[s].x == 1) break;
        float4 c = Op(cop[s].x, Arg(cop[s].y, cur, dif, spc, tex[s], tmp), Arg(cop[s].z, cur, dif, spc, tex[s], tmp),
                      Arg(cop[s].w, cur, dif, spc, tex[s], tmp), cur, dif, tex[s], true);
        float a = cur.a;
        if (aop[s].x != 1)
            a = Op(aop[s].x, Arg(aop[s].y, cur, dif, spc, tex[s], tmp), Arg(aop[s].z, cur, dif, spc, tex[s], tmp),
                   Arg(aop[s].w, cur, dif, spc, tex[s], tmp), cur, dif, tex[s], false).a;
        if (stg[s].x == 5) tmp = float4(c.rgb, a);
        else cur = float4(c.rgb, a);
    }
    if (pmisc.z != 0) cur.rgb = saturate(cur.rgb + spc.rgb);
    if (pmisc.y != 0) {
        float f = i.fog;
        if (pmisc.w != 0) {
            float z = i.pos.w;
            if (pmisc.w == 3) f = (fogtable.y - z) / max(fogtable.y - fogtable.x, 1e-20);
            else if (pmisc.w == 1) f = exp(-z * fogtable.z);
            else f = exp(-(z * fogtable.z) * (z * fogtable.z));
        }
        cur.rgb = lerp(fogcolor.rgb, cur.rgb, saturate(f));
    }
    if (pmisc.x != 8) {
        float a = floor(saturate(cur.a) * 255 + 0.5), r = alpharef.x;
        if (pmisc.x >= 16) {
            /* alpha to coverage: coverage rises from 0 to 1 across about one pixel around the test's threshold */
            float c = saturate(cur.a) * 255;
            cur.a = saturate((c - r) / max(fwidth(c), 0.0001) + 0.5);
        } else {
            bool ok = pmisc.x == 2 ? a < r : pmisc.x == 3 ? a == r : pmisc.x == 4 ? a <= r : pmisc.x == 5 ? a > r :
                      pmisc.x == 6 ? a != r : pmisc.x == 7 ? a >= r : false;
            if (!ok) discard;
        }
    }
    return cur;
}
