Texture2D<float4> splashTexture : register(t0);
SamplerState _splashTexture_sampler : register(s0);

static float4 outColor;
static float2 fragUV;

struct SPIRV_Cross_Input
{
    float2 fragUV : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    outColor = float4(splashTexture.Sample(_splashTexture_sampler, fragUV).xyz, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragUV = stage_input.fragUV;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
