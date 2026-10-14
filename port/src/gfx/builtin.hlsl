// Built-in shaders of the renderer (present blit, texel copy). Compiled ahead of time into
// builtin_shaders.inc by tools/compile_builtin_shaders.py (no shader compiler at run time).
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
void vsmain(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
// Downsampling resolve of the supersampled frame: 4x4 bilinear taps spread over the source
// footprint of the output pixel, averaged in linear light (box filter, any scale ratio).
float4 psmain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 fp = float2(ddx(uv).x, ddy(uv).y);
    uint w, h;
    t0.GetDimensions(w, h);
    float2 ratio = abs(fp) * float2(w, h);
    if (max(ratio.x, ratio.y) <= 1.01) return float4(t0.Sample(s0, uv).rgb, 1);
    float3 c = 0;
    [unroll] for (int y = 0; y < 4; y++)
        [unroll] for (int x = 0; x < 4; x++)
            c += pow(abs(t0.SampleLevel(s0, uv + fp * ((float2(x, y) + 0.5) / 4 - 0.5), 0).rgb), 2.2);
    return float4(pow(c / 16, 1 / 2.2), 1);
}
// Texel copy between surfaces that CopySubresourceRegion cannot copy (depth into a color
// surface, different supersampling): source texel = target pixel * ratio + offset.
cbuffer copy : register(b0) { float2 copy_ratio; float2 copy_offset; };
float4 copyps(float4 pos : SV_Position) : SV_Target {
    return t0.Load(int3(floor(pos.xy) * copy_ratio + copy_offset, 0));
}
