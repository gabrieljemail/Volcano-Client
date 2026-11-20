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

static float fragSkyLight;
static float2 fragUV;
static uint fragLayer;
static uint fragBiomeTint;
static float3 fragViewOffset;
static float fragBlockLight;
static float fragFoliageHeight;
static float4 outColor;
static float fragChunkFade;

struct SPIRV_Cross_Input
{
    float2 fragUV : TEXCOORD0;
    nointerpolation uint fragLayer : TEXCOORD1;
    float fragSkyLight : TEXCOORD2;
    nointerpolation uint fragBiomeTint : TEXCOORD3;
    float fragBlockLight : TEXCOORD4;
    float3 fragViewOffset : TEXCOORD5;
    float fragChunkFade : TEXCOORD6;
    float fragFoliageHeight : TEXCOORD7;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

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
    float4 _169 = float4(uv + float2(-spread.x, -spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _169.xyz, _169.w);
    float4 _188 = float4(uv + float2(spread.x, -spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _188.xyz, _188.w);
    float4 _207 = float4(uv + float2(-spread.x, spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _207.xyz, _207.w);
    float4 _225 = float4(uv + float2(spread.x, spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _225.xyz, _225.w);
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

float3 WaterSurface(float3 viewOffset, inout float alpha, out float reflection)
{
    reflection = 0.0f;
    bool _314 = camera_specularStrength <= 0.0f;
    bool _321;
    if (!_314)
    {
        _321 = camera_lightingEnabled < 0.5f;
    }
    else
    {
        _321 = _314;
    }
    if (_321)
    {
        return 0.0f.xxx;
    }
    float3 normal = normalize(cross(ddx(viewOffset), ddy(viewOffset)));
    float3 viewDir = normalize(-viewOffset);
    if (dot(normal, viewDir) < 0.0f)
    {
        normal = -normal;
    }
    float3 world = viewOffset + camera_cameraPosition.xyz;
    float flatness = smoothstep(0.699999988079071044921875f, 0.949999988079071044921875f, normal.y);
    float2 ripple = float2(sin((world.x * 2.2999999523162841796875f) + (camera_time * 1.39999997615814208984375f)) + sin((world.z * 1.7000000476837158203125f) - (camera_time * 1.10000002384185791015625f)), sin((world.z * 2.099999904632568359375f) + (camera_time * 1.2999999523162841796875f)) + sin((world.x * 1.89999997615814208984375f) + (camera_time * 0.89999997615814208984375f)));
    normal = normalize(normal + ((float3(ripple.x, 0.0f, ripple.y) * 0.02500000037252902984619140625f) * flatness));
    float3 sun = camera_sunDirection.xyz;
    float day = smoothstep(-0.0500000007450580596923828125f, 0.1500000059604644775390625f, sun.y);
    float3 lightDir = normalize(lerp(-sun, sun, day.xxx));
    float intensity = lerp(0.25f, 1.0f, day);
    float sky = fragSkyLight * fragSkyLight;
    float3 halfway = normalize(lightDir + viewDir);
    float glint = pow(max(dot(normal, halfway), 0.0f), 160.0f) * max(dot(normal, lightDir), 0.0f);
    float fresnel = pow(1.0f - max(dot(normal, viewDir), 0.0f), 4.0f);
    reflection = (fresnel * sky) * camera_specularStrength;
    alpha = lerp(alpha, 1.0f, (fresnel * 0.5f) * camera_specularStrength);
    return ((((1.0f.xxx * glint) * sky) * intensity) * 1.5f) * camera_specularStrength;
}

float FogFactor(float3 offset)
{
    float _distance = max(length(offset.xz), abs(offset.y));
    return clamp((_distance - camera_fogStart) / max(camera_fogEnd - camera_fogStart, 0.001000000047497451305389404296875f), 0.0f, 1.0f);
}

void frag_main()
{
    float4 texColor = textureArray.Sample(_textureArray_sampler, float3(fragUV, float(fragLayer)));
    if (texColor.w < 0.00999999977648258209228515625f)
    {
        discard;
    }
    float3 tint = 1.0f.xxx;
    bool _520 = fragBiomeTint == 1u;
    bool _526;
    if (!_520)
    {
        _526 = fragBiomeTint == 3u;
    }
    else
    {
        _526 = _520;
    }
    if (_526)
    {
        tint = float3(0.56862747669219970703125f, 0.74117648601531982421875f, 0.3490196168422698974609375f);
    }
    else
    {
        if (fragBiomeTint == 2u)
        {
            tint = float3(0.24705882370471954345703125f, 0.4627451002597808837890625f, 0.894117653369903564453125f);
        }
        else
        {
            if (fragBiomeTint == 4u)
            {
                tint = float3(0.3803921639919281005859375f, 0.60000002384185791015625f, 0.3803921639919281005859375f);
            }
            else
            {
                if (fragBiomeTint == 5u)
                {
                    tint = float3(0.501960813999176025390625f, 0.654901981353759765625f, 0.3333333432674407958984375f);
                }
                else
                {
                    if (fragBiomeTint == 6u)
                    {
                        tint = float3(0.12549020349979400634765625f, 0.501960813999176025390625f, 0.18823529779911041259765625f);
                    }
                }
            }
        }
    }
    float skyLevel = fragSkyLight;
    float3 param = fragViewOffset;
    float3 param_1 = 0.0f.xxx;
    float param_2 = 0.0f;
    skyLevel *= lerp(1.0f - camera_shadowParams.x, 1.0f, ShadowFactor(param, param_1, param_2));
    float param_3 = skyLevel;
    float param_4 = camera_lightFalloff;
    float skyBrightness = lerp(0.0500000007450580596923828125f, 1.0f, LightCurve(param_3, param_4) * camera_skyLightFactor);
    float param_5 = fragBlockLight;
    float param_6 = camera_lightFalloff;
    float blockBrightness = lerp(0.0500000007450580596923828125f, 1.0f, LightCurve(param_5, param_6));
    float ambient = min(skyBrightness, 0.85000002384185791015625f);
    float light = max(ambient, blockBrightness);
    float _619;
    if (camera_lightingEnabled > 0.5f)
    {
        float param_7 = light;
        _619 = lerp(light, NotGamma(param_7), camera_brightnessFactor);
    }
    else
    {
        _619 = 1.0f;
    }
    light = _619;
    float _635;
    if (fragBiomeTint == 3u)
    {
        _635 = lerp(0.550000011920928955078125f, 1.0f, smoothstep(0.0f, 1.0f, fragFoliageHeight));
    }
    else
    {
        _635 = 1.0f;
    }
    float foliageShade = _635;
    float3 color = ((texColor.xyz * tint) * light) * foliageShade;
    float alpha = texColor.w;
    if (fragBiomeTint == 2u)
    {
        float3 param_8 = fragViewOffset;
        float param_9 = alpha;
        float param_10;
        float3 _668 = WaterSurface(param_8, param_9, param_10);
        alpha = param_9;
        float reflection = param_10;
        float3 glint = _668;
        color = lerp(color, camera_fogColor.xyz, (reflection * 0.60000002384185791015625f).xxx) + glint;
    }
    float3 param_11 = fragViewOffset;
    float3 foggedColor = lerp(color, camera_fogColor.xyz, FogFactor(param_11).xxx);
    outColor = float4(lerp(camera_fogColor.xyz, foggedColor, fragChunkFade.xxx), alpha);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragSkyLight = stage_input.fragSkyLight;
    fragUV = stage_input.fragUV;
    fragLayer = stage_input.fragLayer;
    fragBiomeTint = stage_input.fragBiomeTint;
    fragViewOffset = stage_input.fragViewOffset;
    fragBlockLight = stage_input.fragBlockLight;
    fragFoliageHeight = stage_input.fragFoliageHeight;
    fragChunkFade = stage_input.fragChunkFade;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
