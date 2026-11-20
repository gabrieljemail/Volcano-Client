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

Texture2DArray<float4> shadowMap : register(t0);
SamplerComparisonState _shadowMap_sampler : register(s0);
Texture2DArray<float4> textureArray : register(t0);
SamplerState _textureArray_sampler : register(s0);

static float4 fragSpecular;
static float fragSkyLight;
static float2 fragUV;
static uint fragLayer;
static uint fragBiomeTinted;
static float3 fragViewOffset;
static float fragBlockLight;
static float fragAOMultiplier;
static float fragFaceShade;
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

float ShadowFactor(float3 viewOffset, float3 normal, float facing)
{
    float _distance = -dot(float3(camera_view[0].z, camera_view[1].z, camera_view[2].z), viewOffset);
    int cascade = int(step(camera_shadowSplits.x, _distance) + step(camera_shadowSplits.y, _distance));
    float texelWorld = camera_shadowTexels[cascade];
    float3 world = (viewOffset + camera_cameraPosition.xyz) + (normal * (texelWorld * 1.2000000476837158203125f));
    float4 clip = mul(float4(world, 1.0f), camera_shadowViewProj[cascade]);
    float2 uv = (clip.xy * 0.5f) + 0.5f.xx;
    float biasBlocks = ((texelWorld * 0.699999988079071044921875f) * (1.0f + (2.0f * (1.0f - facing)))) + 0.02999999932944774627685546875f;
    float depth = clip.z - (biasBlocks / camera_shadowRanges[cascade]);
    float texel = camera_shadowParams.y;
    float lit = 0.0f;
    float2 spread = 0.25f.xx * texel;
    float4 _172 = float4(uv + float2(-spread.x, -spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _172.xyz, _172.w);
    float4 _191 = float4(uv + float2(spread.x, -spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _191.xyz, _191.w);
    float4 _210 = float4(uv + float2(-spread.x, spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _210.xyz, _210.w);
    float4 _228 = float4(uv + float2(spread.x, spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _228.xyz, _228.w);
    lit *= 0.25f;
    float inside = ((((step(0.0f, uv.x) * step(uv.x, 1.0f)) * step(0.0f, uv.y)) * step(uv.y, 1.0f)) * step(clip.z, 1.0f)) * (1.0f - step(camera_shadowSplits.z, _distance));
    return lerp(1.0f, lit, inside);
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
    bool _321 = shininess <= 0.0f;
    bool _329;
    if (!_321)
    {
        _329 = camera_specularStrength <= 0.0f;
    }
    else
    {
        _329 = _321;
    }
    bool _336;
    if (!_329)
    {
        _336 = camera_lightingEnabled < 0.5f;
    }
    else
    {
        _336 = _329;
    }
    if (_336)
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
    float sunFacing = dot(fragSpecular.xyz, camera_sunDirection.xyz);
    float3 param_1 = fragViewOffset;
    float3 param_2 = fragSpecular.xyz;
    float param_3 = max(sunFacing, 0.0f);
    float shadow = ShadowFactor(param_1, param_2, param_3) * step(0.001000000047497451305389404296875f, sunFacing);
    skyLevel *= lerp(1.0f - camera_shadowParams.x, 1.0f, shadow);
    float param_4 = skyLevel;
    float param_5 = camera_lightFalloff;
    float skyBrightness = lerp(0.0500000007450580596923828125f, 1.0f, LightCurve(param_4, param_5) * camera_skyLightFactor);
    float param_6 = fragBlockLight;
    float param_7 = camera_lightFalloff;
    float blockBrightness = lerp(0.0500000007450580596923828125f, 1.0f, LightCurve(param_6, param_7));
    float ambient = min(skyBrightness, 0.85000002384185791015625f);
    float light = max(ambient, blockBrightness);
    light *= fragAOMultiplier;
    float _534;
    if (camera_lightingEnabled > 0.5f)
    {
        float param_8 = light;
        _534 = lerp(light, NotGamma(param_8), camera_brightnessFactor);
    }
    else
    {
        _534 = 1.0f;
    }
    light = _534;
    float3 param_9 = fragViewOffset;
    float3 color = (((texColor.xyz * tint) * light) * fragFaceShade) + Specular(param_9);
    if (camera_shadowParams.w > 0.5f)
    {
        bool outside = (-dot(float3(camera_view[0].z, camera_view[1].z, camera_view[2].z), fragViewOffset)) > camera_shadowSplits.z;
        float3 _582;
        if (outside)
        {
            _582 = lerp(color, float3(0.0f, 0.20000000298023223876953125f, 1.0f), 0.60000002384185791015625f.xxx);
        }
        else
        {
            _582 = lerp(float3(1.0f, 0.0f, 1.0f), color, shadow.xxx);
        }
        color = _582;
    }
    float3 param_10 = fragViewOffset;
    float3 foggedColor = lerp(color, camera_fogColor.xyz, FogFactor(param_10).xxx);
    outColor = float4(lerp(camera_fogColor.xyz, foggedColor, fragChunkFade.xxx), texColor.w);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragSpecular = stage_input.fragSpecular;
    fragSkyLight = stage_input.fragSkyLight;
    fragUV = stage_input.fragUV;
    fragLayer = stage_input.fragLayer;
    fragBiomeTinted = stage_input.fragBiomeTinted;
    fragViewOffset = stage_input.fragViewOffset;
    fragBlockLight = stage_input.fragBlockLight;
    fragAOMultiplier = stage_input.fragAOMultiplier;
    fragFaceShade = stage_input.fragFaceShade;
    fragChunkFade = stage_input.fragChunkFade;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
