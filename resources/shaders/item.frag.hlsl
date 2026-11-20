Texture2DArray<float4> textureArray : register(t0);
SamplerState _textureArray_sampler : register(s0);

static float2 fragUV;
static float fragLayer;
static float4 outColor;
static float fragLight;

struct SPIRV_Cross_Input
{
    float2 fragUV : TEXCOORD0;
    float fragLayer : TEXCOORD1;
    float fragLight : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    float4 texColor = textureArray.Sample(_textureArray_sampler, float3(fragUV, fragLayer));
    if (texColor.w < 0.100000001490116119384765625f)
    {
        discard;
    }
    outColor = float4(texColor.xyz * fragLight, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragUV = stage_input.fragUV;
    fragLayer = stage_input.fragLayer;
    fragLight = stage_input.fragLight;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
