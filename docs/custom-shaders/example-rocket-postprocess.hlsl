// Rocket-R Graphics v9 custom final-VI shader example.
// Compile this file to .dxil for D3D12 and/or .spv for Vulkan.

struct VideoInterfaceCB {
    float2 videoResolution;
    float2 textureResolution;
    float gamma;
    uint rocketViFilterMode;
    float rocketDebandStrength;
    uint rocketPostProcessMode;
    float rocketPostProcessStrength;
};

[[vk::push_constant]] ConstantBuffer<VideoInterfaceCB> gConstants : register(b0);
Texture2D<float4> gInput : register(t1);
SamplerState gSampler : register(s2);

float4 SampleRocketVI(float2 uv) {
    const float2 lowerRight = gConstants.videoResolution / gConstants.textureResolution;
    const float2 halfPixel = float2(0.5f, 0.5f) / gConstants.textureResolution;
    const float2 sampleUV = clamp(uv, halfPixel, lowerRight - halfPixel);
    float4 color = gInput.SampleLevel(gSampler, sampleUV, 0.0f);
    color.rgb = pow(max(color.rgb, 0.0f), gConstants.gamma);
    color.a = 1.0f;
    return color;
}

float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
    const float2 sourceUV = (uv / gConstants.textureResolution) * gConstants.videoResolution;
    float4 color = SampleRocketVI(sourceUV);

    // Example: subtle warm lift. Replace with your own single-pass effect.
    const float strength = saturate(gConstants.rocketPostProcessStrength);
    const float3 lifted = color.rgb * float3(1.025f, 1.000f, 0.975f);
    color.rgb = lerp(color.rgb, lifted, strength);
    return saturate(color);
}
