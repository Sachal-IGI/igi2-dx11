// Neonix renderer (Direct3D 11): Direct3D 8's fixed-function vertex processing, one shader for every FVF.
// Elements an FVF lacks come from a small defaults buffer (input slot 15, stride 0); what the FVF has is in NxObj.
// Compiled ahead of time by tools/build_renderer_shaders.py into dx11_shader_code.h.
#include "nx_common.hlsli"

struct NxLight { float4 diffuse, specular, ambient, position, direction, atten, spot; };
cbuffer NxObj : register(b0) {
    row_major float4x4 wv; row_major float4x4 wvp; row_major float4x4 wvn;
    int4 fvf;          // x: pre-transformed (XYZRHW), y: has a normal, z: has a diffuse colour, w: has a specular colour
    int4 texsize0;     // values in texture coordinates 0..3 (0: none)
    int4 texsize1;     // 4..7
};
cbuffer NxFF : register(b4) {
    row_major float4x4 texm[8];
    float4 mat_diffuse, mat_ambient, mat_specular, mat_emissive;
    float4 ambient;
    float4 fogp;       // start, end, density, vertex fog mode (0 none, 1 exp, 2 exp2, 3 linear)
    float4 misc;       // material power, fog on, range fog, table fog mode
    int4 flags;        // lighting, normalize normals, colour vertex, specular
    int4 sources;      // diffuse, specular, ambient, emissive material sources
    int4 nlights;
    int4 tci[8];       // coordinate index, texgen (0 passthru, 1 normal, 2 position, 3 reflection), transform count, projected
    float4 rhw;        // XYZRHW to clip space
    float4 fix;
    NxLight light[8];
};

struct NxIn {
    float4 pos : POSITION;
    float3 normal : NORMAL;
    float4 diffuse : COLOR0;
    float4 specular : COLOR1;
    float4 tex0 : TEXCOORD0; float4 tex1 : TEXCOORD1; float4 tex2 : TEXCOORD2; float4 tex3 : TEXCOORD3;
    float4 tex4 : TEXCOORD4; float4 tex5 : TEXCOORD5; float4 tex6 : TEXCOORD6; float4 tex7 : TEXCOORD7;
};

// D3D: a texture coordinate of n values enters the texture matrix as (.., 1, 0..)
float4 Pad(float4 t, int n)
{
    if (n == 1) return float4(t.x, 1, 0, 0);
    if (n == 2) return float4(t.xy, 1, 0);
    if (n == 3) return float4(t.xyz, 1);
    return t;
}

// D3DRS_*MATERIALSOURCE: a vertex colour only when the FVF has it, else the material
float4 Source(int s, float4 mat, float4 c1, float4 c2)
{
    if (s == 1 && fvf.z != 0) return c1;
    if (s == 2 && fvf.w != 0) return c2;
    return mat;
}

NxVSOut main(NxIn i)
{
    NxVSOut o;
    float4 c1 = i.diffuse, c2 = i.specular;   // white and black when the FVF has none
    float3 pv = float3(0, 0, 0);
    float3 nv = float3(0, 0, 0);
    [branch] if (fvf.x != 0) {
        float w = i.pos.w != 0 ? 1.0 / i.pos.w : 1.0;
        o.pos = float4((i.pos.x * rhw.x + rhw.z) * w, (i.pos.y * rhw.y + rhw.w) * w, i.pos.z * w, w);
        o.d0 = c1;
        o.d1 = c2;
        o.fog = (misc.y != 0) ? c2.a : 1.0;
    } else {
        float4 p = float4(i.pos.xyz, 1);
        pv = mul(p, wv).xyz;
        o.pos = mul(p, wvp);
        o.pos.xy += fix.xy * o.pos.w;
        if (fvf.y != 0) {
            nv = mul(float4(i.normal, 0), wvn).xyz;
            if (flags.y != 0) nv = normalize(nv);
        }
        bool cv = flags.z != 0;   // D3DRS_COLORVERTEX
        float4 dif = cv ? Source(sources.x, mat_diffuse, c1, c2) : mat_diffuse;
        float4 spc = cv ? Source(sources.y, mat_specular, c1, c2) : mat_specular;
        float4 amb = cv ? Source(sources.z, mat_ambient, c1, c2) : mat_ambient;
        float4 emi = cv ? Source(sources.w, mat_emissive, c1, c2) : mat_emissive;
        [branch] if (flags.x != 0) {
            float3 la = ambient.rgb, ld = float3(0, 0, 0), ls = float3(0, 0, 0);
            float3 eye = -normalize(pv);
            for (int k = 0; k < nlights.x; k++) {
                NxLight L = light[k];
                float3 dir;
                float att = 1;
                if (L.spot.w == 3) {
                    dir = L.direction.xyz;
                } else {
                    float3 d = L.position.xyz - pv;
                    float dist = length(d);
                    dir = d / max(dist, 1e-20);
                    if (dist > L.atten.x) att = 0;
                    else att = 1.0 / max(L.atten.y + L.atten.z * dist + L.atten.w * dist * dist, 1e-20);
                    if (L.spot.w == 2) {
                        float rho = dot(-dir, -L.direction.xyz);
                        if (rho <= L.spot.z) att = 0;
                        else if (rho <= L.spot.y) att *= pow(saturate((rho - L.spot.z) / max(L.spot.y - L.spot.z, 1e-20)), L.spot.x);
                    }
                }
                la += L.ambient.rgb * att;
                float nl = dot(nv, dir);
                if (nl > 0) {
                    ld += L.diffuse.rgb * nl * att;
                    if (flags.w != 0) {
                        float nh = dot(nv, normalize(dir + eye));
                        if (nh > 0) ls += L.specular.rgb * pow(nh, misc.x) * att;
                    }
                }
            }
            o.d0 = float4(saturate(emi.rgb + amb.rgb * la + dif.rgb * ld), dif.a);
            o.d1 = float4(saturate(spc.rgb * ls), spc.a);
        } else {
            o.d0 = c1;
            o.d1 = c2;
        }
        float fz = (misc.z != 0) ? length(pv) : pv.z;
        float f = 1;
        if (misc.y != 0 && misc.w == 0) {
            if (fogp.w == 3) f = (fogp.y - fz) / max(fogp.y - fogp.x, 1e-20);
            else if (fogp.w == 1) f = exp(-fz * fogp.z);
            else if (fogp.w == 2) f = exp(-(fz * fogp.z) * (fz * fogp.z));
            else f = c2.a;
        }
        o.fog = saturate(f);
    }
    float4 tin[8];
    tin[0] = Pad(i.tex0, texsize0.x); tin[1] = Pad(i.tex1, texsize0.y); tin[2] = Pad(i.tex2, texsize0.z); tin[3] = Pad(i.tex3, texsize0.w);
    tin[4] = Pad(i.tex4, texsize1.x); tin[5] = Pad(i.tex5, texsize1.y); tin[6] = Pad(i.tex6, texsize1.z); tin[7] = Pad(i.tex7, texsize1.w);
    float4 tc[8];
    [unroll] for (int s = 0; s < 8; s++) {
        float4 t;
        if (tci[s].y == 1) t = float4(nv, 1);
        else if (tci[s].y == 2) t = float4(pv, 1);
        else if (tci[s].y == 3) t = float4(reflect(normalize(pv), nv), 1);
        else t = tin[tci[s].x & 7];
        if (tci[s].z != 0) t = mul(t, texm[s]);
        tc[s] = t;
    }
    o.t0 = tc[0]; o.t1 = tc[1]; o.t2 = tc[2]; o.t3 = tc[3]; o.t4 = tc[4]; o.t5 = tc[5]; o.t6 = tc[6]; o.t7 = tc[7];
    return o;
}
