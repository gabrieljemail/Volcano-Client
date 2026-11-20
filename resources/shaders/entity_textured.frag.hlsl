cbuffer CameraUBO : register(b0)
{
    row_major float4x4 camera_view : packoffset(c0);
    row_major float4x4 camera_proj : packoffset(c4);
    float camera_lightingEnabled : packoffset(c8);
    float camera_brightnessFactor : packoffset(c8.y);
    float camera_skyLightFactor : packoffset(c8.z);
    float camera_lightFalloff : packoffset(c8.w);
    float camera_shadeTop : packoffset(c9);
    float camera_shadeBottom : packoffset(c9.y);
    float camera_shadeNorthSouth : packoffset(c9.z);
    float camera_shadeEastWest : packoffset(c9.w);
    float4 camera_fogColor : packoffset(c10);
    float4 camera_cameraPosition : packoffset(c11);
    float camera_fogStart : packoffset(c12);
    float camera_fogEnd : packoffset(c12.y);
    float camera_specularStrength : packoffset(c12.z);
    float camera_time : packoffset(c12.w);
    float4 camera_sunDirection : packoffset(c13);
};

Texture2D<float4> skinTexture : register(t0);
SamplerState _skinTexture_sampler : register(s0);

static float2 fragUV;
static float3 fragNormal;
static float2 fragLight;
static float3 fragTint;
static float4 fragHurt;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float3 fragNormal : TEXCOORD0;
    float2 fragUV : TEXCOORD1;
    float4 fragHurt : TEXCOORD2;
    nointerpolation float2 fragLight : TEXCOORD4;
    nointerpolation float3 fragTint : TEXCOORD5;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

float LightCurve(float level, float falloff)
{
    return level / (1.0f + (falloff * (1.0f - level)));
}

float NotGamma(float level)
{
    float inverted = 1.0f - level;
    return 1.0f - (((inverted * inverted) * inverted) * inverted);
}

void frag_main()
{
    float4 texColor = skinTexture.Sample(_skinTexture_sampler, fragUV);
    if (texColor.w < 0.5f)
    {
        discard;
    }
    float diffuse = max(dot(normalize(fragNormal), float3(0.3837999999523162841796875f, 0.78450000286102294921875f, 0.48849999904632568359375f)), 0.0f);
    float brightness = 0.4000000059604644775390625f + (0.60000002384185791015625f * diffuse);
    float param = fragLight.x;
    float param_1 = camera_lightFalloff;
    float skyBrightness = lerp(0.0500000007450580596923828125f, 1.0f, LightCurve(param, param_1) * camera_skyLightFactor);
    float param_2 = fragLight.y;
    float param_3 = camera_lightFalloff;
    float blockBrightness = lerp(0.0500000007450580596923828125f, 1.0f, LightCurve(param_2, param_3));
    float light = max(min(skyBrightness, 0.85000002384185791015625f), blockBrightness);
    float _126;
    if (camera_lightingEnabled > 0.5f)
    {
        float param_4 = light;
        _126 = lerp(light, NotGamma(param_4), camera_brightnessFactor);
    }
    else
    {
        _126 = 1.0f;
    }
    light = _126;
    float3 shaded = ((texColor.xyz * fragTint) * brightness) * light;
    float3 finalColor = lerp(shaded, fragHurt.xyz, fragHurt.w.xxx);
    outColor = float4(finalColor, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragUV = stage_input.fragUV;
    fragNormal = stage_input.fragNormal;
    fragLight = stage_input.fragLight;
    fragTint = stage_input.fragTint;
    fragHurt = stage_input.fragHurt;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
