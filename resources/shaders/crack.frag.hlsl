Texture2D<float4> crackTexture : register(t0);
SamplerState _crackTexture_sampler : register(s0);

static float2 fragUV;
static float4 outColor;

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
    float4 crack = crackTexture.Sample(_crackTexture_sampler, fragUV);
    outColor = float4(lerp(0.5f.xxx, crack.xyz, crack.w.xxx), 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragUV = stage_input.fragUV;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
