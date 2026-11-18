// Neonix renderer (Direct3D 11): igi2's terrain drawn on the GPU (dx11_terrain.c), one draw per height map.
// What the game builds on the CPU each frame (TerrainRender_Draw 0x609610): a vertex per height sample at (x, y, height),
// fog per vertex, the game's own position math (its terrain vertex shaders: m3x4 r1, v0, c0; add oPos, r1, c14: x, y, z
// only; v0.w is the fog), and per material block a weight (stream 1) that its passes add up. Colour: each block's layer
// (two textures, planar coordinates) times its weight with the fog towards half the fog colour, then twice the height
// map's lightmap with the fog towards white (TerrainRender_SetPassStates 0x606570).
// Geometry: full detail near the camera, a grid twice as coarse per step of distance, vertices sliding between steps
// (dx11_terrain.c Select). The material weights follow the game's own level of detail, because that is what makes its
// material edges soft at a distance (see MaterialWeights).

cbuffer TerrainCB : register(b6)
{
    float4 c[4];        // the game's terrain vertex shader constants c0..c3 (m3x4: xyz of each)
    float4 c14;         // c14
    float4 fix;         // half pixel (D3D8 to D3D11): xy times w added to the position; z: dev zoom (1 = none);
                        // w: 1 = squared fog exponent ([TerrainFog] Exp2Levels)
    float4 fogp;        // fog = exp2(-dot(fogp, float4(p, 1))): the game's fog plane times density times log2(e)
    float4 foghalf;     // rgb: the layer passes' fog colour, w: fog on
    float4 misc;        // x: lightmap pass on, y: holes, z: later regions, w: the coarsest level (no sliding beyond it)
    float4 grid;        // this map: x, y of sample 0, cell size x, y
    float4 size;        // this map: samples w, h (sample w and h are extrapolated), z: levels of detail (log2 of the root),
                        // w: R0, the distance (terrain units) out to which the geometry is at full detail
    float4 pgrid;       // the map this one lies in (its border follows that map's surface): x0, y0, cell x, cell y
    float4 psize;       // its samples w, h; z: 1 when there is such a map
    float4 region;      // where this map draws: xmin, xmax, ymin, ymax
    float4 later[10];   // regions of later (finer) maps, not drawn here
    float4 lm;          // lightmap: uv = (p.xy - lm.xy) * lm.zw
    float4 layer[16];   // per block: x uv0 scale, y uv1 scale, z axis (0 xy, 1 xz, 2 yz), w 1 when the block is drawn
    float4 holes[64];   // DiscardTask rectangles: xmin, xmax, ymin, ymax
    float4 lod;         // x: split factor k (ctx + 36), y: height error factor (ctx + 28) / 2, z: 1 = level of detail on,
                        // w: dev view (nx_terrain_debug.txt)
    float4 cam;         // camera position (terrain units)
};

Texture2D<float> hmap : register(t0);
Texture2D<uint> mmap : register(t1);
Texture2D<float> pmap : register(t2);
Texture2D lt0[16] : register(t16);
Texture2D lt1[16] : register(t32);
Texture2D lmap : register(t48);
SamplerState s0 : register(s8);
SamplerState s1 : register(s9);
SamplerState sl : register(s10);

struct TerrainOut {
    float4 pos : SV_Position;
    float3 p : TEXCOORD0;      // terrain position
    float fog : TEXCOORD1;
    float hole : TEXCOORD2;
    float4 w0 : TEXCOORD3;     // material block weights 0..15 (the game's, per vertex as it does)
    float4 w1 : TEXCOORD4;
    float4 w2 : TEXCOORD5;
    float4 w3 : TEXCOORD6;
};

// a height sample; past the last column or row extrapolated as the game does (sub_609520)
float H(Texture2D<float> t, int2 s, int2 n)
{
    if (s.x < n.x && s.y < n.y) return t.Load(int3(s, 0));
    if (s.x < n.x) return 2 * t.Load(int3(s.x, n.y - 1, 0)) - t.Load(int3(s.x, n.y - 2, 0));
    if (s.y < n.y) return 2 * t.Load(int3(n.x - 1, s.y, 0)) - t.Load(int3(n.x - 2, s.y, 0));
    return 3 * t.Load(int3(n.x - 1, n.y - 1, 0)) - t.Load(int3(n.x - 2, n.y - 1, 0)) - t.Load(int3(n.x - 1, n.y - 2, 0));
}


// the game's layer shaders (gShadersA, sources at 0x6BD6B0, 0x6BD638, 0x6BD5C0): 0 = v0.xy (from above), 1 = v0.xz, 2 = v0.yz
float2 Axis(float3 p, float a)
{
    return a < 0.5 ? p.xy : a < 1.5 ? p.xz : p.yz;
}

uint M(int2 s, int2 n) { return mmap.Load(int3(min(s, n - 1), 0)); }

// the grid of level j (nodes every 2^j samples): its cell around g and where g lies in it
void Cell(float2 g, float j, out int2 a, out int st, out float2 f)
{
    st = 1 << (int)j;
    a = int2(floor(g / st)) * st;
    f = saturate((g - float2(a)) / st);
}

// height of the level-j surface at g (the game's coarser triangles, as a bilinear patch)
float HLevel(float2 g, float j, int2 n)
{
    int2 a; int st; float2 f;
    Cell(g, j, a, st, f);
    float h00 = H(hmap, min(a, n), n), h10 = H(hmap, min(a + int2(st, 0), n), n);
    float h01 = H(hmap, min(a + int2(0, st), n), n), h11 = H(hmap, min(a + int2(st, st), n), n);
    return lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
}

// adds the level-j weights times k: each node of that grid carries its own sample's material (weight 1), blended
// across the cell as the game's vertex weights are across its triangles
void AddLevel(inout float w[16], float2 g, float j, int2 n, float k)
{
    int2 a; int st; float2 f;
    Cell(g, j, a, st, f);
    w[M(a, n) & 15] += k * (1 - f.x) * (1 - f.y);
    w[M(a + int2(st, 0), n) & 15] += k * f.x * (1 - f.y);
    w[M(a + int2(0, st), n) & 15] += k * (1 - f.x) * f.y;
    w[M(a + int2(st, st), n) & 15] += k * f.x * f.y;
}

// The game's level of detail (TerrainRender_Draw): an edge of length E (terrain units) is split when E > K d, d the
// camera's distance to it, K = ctx[36] / (1 + e ctx[28] / 2), e = |mean of the edge's end heights - the height at its
// middle| / E. The new middle vertex's weights morph from the mean of the ends' (the coarser level) to its own sample's
// material as v = (E / (K d))^2 - 1 (0..1; 0x6063B0). So at a distance d the finest grid has nodes every 2^J samples with
// 2^(J+1) cells > K d / cell >= 2^J cells, and its new nodes are morphed by v from level J + 1.
void MaterialWeights(float3 p, int2 n, out float w[16])
{
    [unroll] for (int b = 0; b < 16; b++) w[b] = 0;
    float2 g = (p.xy - grid.xy) / grid.zw;
    float cell = grid.z;
    if (lod.z <= 0) { AddLevel(w, g, 0, n, 1); return; }
    float d = max(length(p - cam.xyz), 1);
    float top = size.z;
    float r = lod.x * d / cell;                    // K d in cells, the error not counted yet
    float j = clamp(floor(log2(max(r, 1))), 0, top);
    if (lod.y > 0 && j < top) {
        // the height error at this level makes K smaller (finer near bumps): once, with the error of level j's edges
        float e = abs(HLevel(g, j + 1, n) - HLevel(g, j, n)) / (exp2(j + 1) * cell);
        r = lod.x / (1 + e * lod.y) * d / cell;
        j = clamp(floor(log2(max(r, 1))), 0, top);
    }
    float E = exp2(j + 1);
    float v = j < top ? saturate(E * E / (r * r) - 1) : 1;
    AddLevel(w, g, j, n, v);
    if (v < 1) AddLevel(w, g, j + 1, n, 1 - v);
}

// node: x = first sample x | level << 16, y = first sample y | quarter node << 16
TerrainOut TerrainVS(uint id : SV_VertexID, uint2 node : CHUNK)
{
    TerrainOut o;
    int2 n = int2(size.xy);
    uint level = node.x >> 16, cells = (node.y >> 16) ? 8 : 16;
    int st = 1 << level;
    int2 org = int2(node.x & 0xFFFF, node.y & 0xFFFF);
    int2 g = int2(id % (cells + 1), id / (cells + 1));
    int2 s = min(org + g * st, n);                 // the vertex's sample
    int2 t = min(org + (g - (g & 1)) * st, n);     // the even one it slides onto (level + 1)
    float hs = H(hmap, s, n);
    float m = 0;
    if (any(s != t) && level < (uint)misc.w) {
        float range = size.w * st, start = (level ? 0.85 : 0.7) * range;
        float d = length(float3(grid.xy + float2(s) * grid.zw, hs) - cam.xyz);
        m = saturate((d - start) / (range - start));
    }
    float2 q = lerp(float2(s), float2(t), m);
    float3 p;
    p.xy = grid.xy + q * grid.zw;
    p.z = lerp(hs, H(hmap, t, n), m);
    if (psize.z > 0 && (q.x <= 0 || q.y <= 0 || q.x >= n.x || q.y >= n.y)) {
        // on the border of a map inside another: that map's surface as it is drawn, its cell's two triangles split from
        // (x + 1, y) to (x, y + 1) (dx11_terrain.c), so the border meets it also where it does not run along its grid
        // lines (a map placed off the outer grid in the editor); along a grid line this is the straight line between
        // the samples, as before
        int2 pn = int2(psize.xy);
        float2 f = (p.xy - pgrid.xy) / pgrid.zw;
        int2 i0 = clamp(int2(floor(f + 1e-4)), int2(0, 0), pn - 1);
        float2 t = saturate(f - float2(i0));
        float a = H(pmap, i0, pn), b = H(pmap, i0 + int2(1, 0), pn);
        float d = H(pmap, i0 + int2(0, 1), pn), e = H(pmap, i0 + int2(1, 1), pn);
        p.z = t.x + t.y <= 1 ? a + (b - a) * t.x + (d - a) * t.y : e + (d - e) * (1 - t.x) + (b - e) * (1 - t.y);
    }
    // The game's fog: e^-x, x = depth x density; here y = x log2(e), so exp2(-y). [TerrainFog] Exp2Levels squares x first
    // (terrain_abi.S on the CPU path): e^-(x^2) = exp2(-y^2 ln 2).
    float y = dot(fogp, float4(p, 1));
    float fog = fix.w > 0 ? exp2(-y * y * 0.69314718) : exp2(-y);
    o.pos = float4(dot(p, c[0].xyz), dot(p, c[1].xyz), dot(p, c[2].xyz), dot(p, c[3].xyz)) + c14;   // m3x4: fog is not part of it
    o.pos.xy += fix.xy * o.pos.w;
    if (fix.z > 1) o.pos.xy *= fix.z;   // dev: magnified like binoculars
    o.p = p;
    o.fog = fog;
    float hole = 0;
    for (int k = 0; k < (int)misc.y; k++) {
        float4 r = holes[k];
        if (p.x >= r.x && p.x <= r.y && p.y >= r.z && p.y <= r.w) hole = 1;
    }
    o.hole = hole;
    float w[16];
    MaterialWeights(p, n, w);
    o.w0 = float4(w[0], w[1], w[2], w[3]);
    o.w1 = float4(w[4], w[5], w[6], w[7]);
    o.w2 = float4(w[8], w[9], w[10], w[11]);
    o.w3 = float4(w[12], w[13], w[14], w[15]);
    return o;
}

float4 TerrainPS(TerrainOut i) : SV_Target
{
    // a triangle with all three corners in a DiscardTask rectangle is not drawn
    if (i.hole > 0.9999) discard;
    float2 q = i.p.xy;
    if (q.x < region.x || q.x > region.y || q.y < region.z || q.y > region.w) discard;
    for (int k = 0; k < (int)misc.z; k++) {
        float4 r = later[k];
        if (q.x > r.x && q.x < r.y && q.y > r.z && q.y < r.w) discard;
    }
    float f = saturate(i.fog);
    bool fogon = foghalf.w > 0;
    float3 dx = ddx(i.p), dy = ddy(i.p);
    float w[16] = {i.w0.x, i.w0.y, i.w0.z, i.w0.w, i.w1.x, i.w1.y, i.w1.z, i.w1.w,
                   i.w2.x, i.w2.y, i.w2.z, i.w2.w, i.w3.x, i.w3.y, i.w3.z, i.w3.w};
    float3 sum = 0;
    if (lod.w == 1) {
        // dev: material colours
        static const float3 pal[8] = {float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), float3(1, 1, 0),
                                      float3(1, 0, 1), float3(0, 1, 1), float3(1, 1, 1), float3(0.5, 0.5, 0.5)};
        [unroll] for (int k = 0; k < 16; k++) sum += w[k] * pal[k & 7];
        return float4(sum, 1);
    }
    if (lod.w == 3) return float4(f, f, f, 1);
    if (lod.w == 4) return float4(lmap.Sample(sl, (q - lm.xy) * lm.zw).rgb, 1);
    [unroll] for (int b = 0; b < 16; b++) {
        [branch] if (w[b] > 0.002 && layer[b].w > 0) {
            float2 a = Axis(i.p, layer[b].z), ax = Axis(dx, layer[b].z), ay = Axis(dy, layer[b].z);
            float3 t0 = lt0[b].SampleGrad(s0, a * layer[b].x, ax * layer[b].x, ay * layer[b].x).rgb;
            float3 t1 = lt1[b].SampleGrad(s1, a * layer[b].y, ax * layer[b].y, ay * layer[b].y).rgb;
            float3 col = lod.w == 5 ? t0 : lod.w == 6 ? t1 : lod.w == 7 ? float3(1, 1, 1) : t0 * t1;
            if (fogon && lod.w != 2 && lod.w < 5) col = lerp(foghalf.rgb, col, f);
            sum += w[b] * col;
        }
    }
    if (lod.w >= 5 || lod.w == 2) return float4(sum, 1);
    sum = saturate(sum);
    if (misc.x > 0) {
        float3 l = lmap.Sample(sl, (q - lm.xy) * lm.zw).rgb;
        if (fogon) l = lerp(float3(1, 1, 1), l, f);
        sum = saturate(2 * l * sum);
    }
    return float4(sum, 1);
}
