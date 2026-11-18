// Neonix renderer (Direct3D 11): the present blit (the game's back buffer into the swap chain, through the gamma ramp) and
// the quad that clears part of a target. Entry points: BlitVS, BlitPS, ClearVS, ClearPS.
// Compiled ahead of time by tools/build_renderer_shaders.py into dx11_shader_code.h.

Texture2D frame : register(t0);
Texture2D ramp : register(t1);
SamplerState sm : register(s0);
cbuffer NxBlit : register(b0) { float4 opt; };   // x: gamma ramp on
cbuffer NxClear : register(b1) { float4 rect; float4 color; float4 depth; };

void BlitVS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 BlitPS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 c = frame.Sample(sm, uv).rgb;
    if (opt.x != 0)
        c = float3(ramp.Load(int3(saturate(c.r) * 255 + 0.5, 0, 0)).r, ramp.Load(int3(saturate(c.g) * 255 + 0.5, 0, 0)).g,
                   ramp.Load(int3(saturate(c.b) * 255 + 0.5, 0, 0)).b);
    return float4(c, 1);
}

float4 ClearVS(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id & 1) ? 1 : 0, (id & 2) ? 1 : 0);
    return float4(lerp(rect.xy, rect.zw, t), depth.x, 1);
}

float4 ClearPS(float4 pos : SV_Position) : SV_Target
{
    return color;
}
