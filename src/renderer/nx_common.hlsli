// Neonix renderer (Direct3D 11): what every vertex shader writes and the pixel shaders read.
// D3D8 puts pixel centres on whole coordinates, D3D11 half a pixel further: vertex shaders move positions by that.
struct NxVSOut {
    float4 pos : SV_Position;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
    float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7;
    float fog : FOG;
};
