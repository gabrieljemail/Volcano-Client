Texture2D<float4> skinTexture : register(t0);
SamplerState _skinTexture_sampler : register(s0);

static float2 fragUV;
static float4 outColor;
static float fragLight;

struct SPIRV_Cross_Input
{
    float2 fragUV : TEXCOORD0;
    float fragLight : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    float4 texColor = skinTexture.Sample(_skinTexture_sampler, fragUV);
    if (texColor.w < 0.5f)
    {
        discard;
    }
    outColor = float4(texColor.xyz * fragLight, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragUV = stage_input.fragUV;
    fragLight = stage_input.fragLight;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
