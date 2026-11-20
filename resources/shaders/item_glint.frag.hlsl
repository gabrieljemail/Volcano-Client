Texture2DArray<float4> textureArray : register(t0);
SamplerState _textureArray_sampler : register(s0);
Texture2D<float4> glintTexture : register(t0);
SamplerState _glintTexture_sampler : register(s0);

static float2 fragUV;
static float fragLayer;
static float fragTime;
static float4 outColor;
static float fragLight;

struct SPIRV_Cross_Input
{
    float2 fragUV : TEXCOORD0;
    float fragLayer : TEXCOORD1;
    float fragLight : TEXCOORD2;
    float fragTime : TEXCOORD3;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    if (textureArray.Sample(_textureArray_sampler, float3(fragUV, fragLayer)).w < 0.100000001490116119384765625f)
    {
        discard;
    }
    float2 scaled = fragUV * 8.0f;
    float2 tilted = float2((0.9848077297210693359375f * scaled.x) - (0.173648178577423095703125f * scaled.y), (0.173648178577423095703125f * scaled.x) + (0.9848077297210693359375f * scaled.y));
    float2 uv = tilted + float2(-frac(fragTime / 27.5f), frac(fragTime / 7.5f));
    outColor = float4(glintTexture.Sample(_glintTexture_sampler, uv).xyz * 0.75f, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragUV = stage_input.fragUV;
    fragLayer = stage_input.fragLayer;
    fragTime = stage_input.fragTime;
    fragLight = stage_input.fragLight;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
