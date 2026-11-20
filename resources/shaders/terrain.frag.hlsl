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
    row_major float4x4 camera_shadowViewProj[3] : packoffset(c14);
    float4 camera_shadowParams : packoffset(c26);
    float4 camera_shadowSplits : packoffset(c27);
    float4 camera_shadowTexels : packoffset(c28);
    float4 camera_shadowRanges : packoffset(c29);
};

Texture2DArray<float4> textureArray : register(t0);
SamplerState _textureArray_sampler : register(s0);

static float4 fragSpecular;
static float fragSkyLight;
static float2 fragUV;
static uint fragLayer;
static uint fragBiomeTinted;
static float fragBlockLight;
static float fragAOMultiplier;
static float fragFaceShade;
static float3 fragViewOffset;
static float4 outColor;
static float fragChunkFade;

struct SPIRV_Cross_Input
{
    float2 fragUV : TEXCOORD0;
    nointerpolation uint fragLayer : TEXCOORD1;
    float fragSkyLight : TEXCOORD2;
    nointerpolation uint fragBiomeTinted : TEXCOORD3;
    float fragBlockLight : TEXCOORD4;
    float fragAOMultiplier : TEXCOORD5;
    float fragFaceShade : TEXCOORD6;
    float3 fragViewOffset : TEXCOORD7;
    float fragChunkFade : TEXCOORD8;
    nointerpolation float4 fragSpecular : TEXCOORD9;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

float3 BiomeColor(uint mode)
{
    if (mode == 1u)
    {
        return float3(0.56862747669219970703125f, 0.74117648601531982421875f, 0.3490196168422698974609375f);
    }
    if (mode == 4u)
    {
        return float3(0.3803921639919281005859375f, 0.60000002384185791015625f, 0.3803921639919281005859375f);
    }
    if (mode == 5u)
    {
        return float3(0.501960813999176025390625f, 0.654901981353759765625f, 0.3333333432674407958984375f);
    }
    return 1.0f.xxx;
}

float LightCurve(float level, float falloff)
{
    return level / (1.0f + (falloff * (1.0f - level)));
}

float NotGamma(float level)
{
    float inverted = 1.0f - level;
    return 1.0f - (((inverted * inverted) * inverted) * inverted);
}

float3 Specular(float3 viewOffset)
{
    float shininess = fragSpecular.w;
    bool _101 = shininess <= 0.0f;
    bool _109;
    if (!_101)
    {
        _109 = camera_specularStrength <= 0.0f;
    }
    else
    {
        _109 = _101;
    }
    bool _118;
    if (!_109)
    {
        _118 = camera_lightingEnabled < 0.5f;
    }
    else
    {
        _118 = _109;
    }
    if (_118)
    {
        return 0.0f.xxx;
    }
    float3 sun = camera_sunDirection.xyz;
    float day = smoothstep(-0.0500000007450580596923828125f, 0.1500000059604644775390625f, sun.y);
    float3 lightDir = normalize(lerp(-sun, sun, day.xxx));
    float intensity = lerp(0.20000000298023223876953125f, 1.0f, day);
    float3 normal = fragSpecular.xyz;
    float3 viewDir = normalize(-viewOffset);
    float3 halfway = normalize(lightDir + viewDir);
    float facing = max(dot(normal, lightDir), 0.0f);
    float highlight = pow(max(dot(normal, halfway), 0.0f), lerp(8.0f, 128.0f, shininess));
    float sky = fragSkyLight * fragSkyLight;
    return (((((1.0f.xxx * highlight) * facing) * sky) * intensity) * lerp(0.1500000059604644775390625f, 1.0f, shininess)) * camera_specularStrength;
}

float FogFactor(float3 offset)
{
    float _distance = max(length(offset.xz), abs(offset.y));
    return clamp((_distance - camera_fogStart) / max(camera_fogEnd - camera_fogStart, 0.001000000047497451305389404296875f), 0.0f, 1.0f);
}

void frag_main()
{
    float4 texColor = textureArray.Sample(_textureArray_sampler, float3(fragUV, float(fragLayer)));
    uint param = fragBiomeTinted;
    float3 tint = BiomeColor(param);
    float skyLevel = fragSkyLight;
    float param_1 = skyLevel;
    float param_2 = camera_lightFalloff;
    float skyBrightness = lerp(0.0500000007450580596923828125f, 1.0f, LightCurve(param_1, param_2) * camera_skyLightFactor);
    float param_3 = fragBlockLight;
    float param_4 = camera_lightFalloff;
    float blockBrightness = lerp(0.0500000007450580596923828125f, 1.0f, LightCurve(param_3, param_4));
    float ambient = min(skyBrightness, 0.85000002384185791015625f);
    float light = max(ambient, blockBrightness);
    light *= fragAOMultiplier;
    float _289;
    if (camera_lightingEnabled > 0.5f)
    {
        float param_5 = light;
        _289 = lerp(light, NotGamma(param_5), camera_brightnessFactor);
    }
    else
    {
        _289 = 1.0f;
    }
    light = _289;
    float3 param_6 = fragViewOffset;
    float3 color = (((texColor.xyz * tint) * light) * fragFaceShade) + Specular(param_6);
    float3 param_7 = fragViewOffset;
    float3 foggedColor = lerp(color, camera_fogColor.xyz, FogFactor(param_7).xxx);
    outColor = float4(lerp(camera_fogColor.xyz, foggedColor, fragChunkFade.xxx), texColor.w);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragSpecular = stage_input.fragSpecular;
    fragSkyLight = stage_input.fragSkyLight;
    fragUV = stage_input.fragUV;
    fragLayer = stage_input.fragLayer;
    fragBiomeTinted = stage_input.fragBiomeTinted;
    fragBlockLight = stage_input.fragBlockLight;
    fragAOMultiplier = stage_input.fragAOMultiplier;
    fragFaceShade = stage_input.fragFaceShade;
    fragViewOffset = stage_input.fragViewOffset;
    fragChunkFade = stage_input.fragChunkFade;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
