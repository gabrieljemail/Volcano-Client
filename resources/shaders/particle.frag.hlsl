Texture2DArray<float4> atlas : register(t0);
SamplerState _atlas_sampler : register(s0);

static float2 inUv;
static float inLayer;
static float4 inColor;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float2 inUv : TEXCOORD0;
    float4 inColor : TEXCOORD1;
    nointerpolation float inLayer : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    float4 color = atlas.Sample(_atlas_sampler, float3(inUv, inLayer)) * inColor;
    if (color.w < 0.0199999995529651641845703125f)
    {
        discard;
    }
    outColor = color;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inUv = stage_input.inUv;
    inLayer = stage_input.inLayer;
    inColor = stage_input.inColor;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
