Texture2DArray<float4> textureArray : register(t0);
SamplerState _textureArray_sampler : register(s0);

static float2 fragUV;
static uint fragLayer;

struct SPIRV_Cross_Input
{
    float2 fragUV : TEXCOORD0;
    nointerpolation uint fragLayer : TEXCOORD1;
};

void frag_main()
{
    if (textureArray.Sample(_textureArray_sampler, float3(fragUV, float(fragLayer))).w < 0.5f)
    {
        discard;
    }
}

void main(SPIRV_Cross_Input stage_input)
{
    fragUV = stage_input.fragUV;
    fragLayer = stage_input.fragLayer;
    frag_main();
}
