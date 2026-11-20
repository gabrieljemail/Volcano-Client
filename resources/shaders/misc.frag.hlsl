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

static float fragSkyLight;
static float2 fragUV;
static uint fragLayer;
static uint fragBiomeTint;
static float fragBlockLight;
static float fragFoliageHeight;
static float3 fragViewOffset;
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
    bool _94 = camera_specularStrength <= 0.0f;
    bool _103;
    if (!_94)
    {
        _103 = camera_lightingEnabled < 0.5f;
    }
    else
    {
        _103 = _94;
    }
    if (_103)
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
    bool _310 = fragBiomeTint == 1u;
    bool _316;
    if (!_310)
    {
        _316 = fragBiomeTint == 3u;
    }
    else
    {
        _316 = _310;
    }
    if (_316)
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
    float param = skyLevel;
    float param_1 = camera_lightFalloff;
    float skyBrightness = lerp(0.0500000007450580596923828125f, 1.0f, LightCurve(param, param_1) * camera_skyLightFactor);
    float param_2 = fragBlockLight;
    float param_3 = camera_lightFalloff;
    float blockBrightness = lerp(0.0500000007450580596923828125f, 1.0f, LightCurve(param_2, param_3));
    float ambient = min(skyBrightness, 0.85000002384185791015625f);
    float light = max(ambient, blockBrightness);
    float _396;
    if (camera_lightingEnabled > 0.5f)
    {
        float param_4 = light;
        _396 = lerp(light, NotGamma(param_4), camera_brightnessFactor);
    }
    else
    {
        _396 = 1.0f;
    }
    light = _396;
    float _412;
    if (fragBiomeTint == 3u)
    {
        _412 = lerp(0.550000011920928955078125f, 1.0f, smoothstep(0.0f, 1.0f, fragFoliageHeight));
    }
    else
    {
        _412 = 1.0f;
    }
    float foliageShade = _412;
    float3 color = ((texColor.xyz * tint) * light) * foliageShade;
    float alpha = texColor.w;
    if (fragBiomeTint == 2u)
    {
        float3 param_5 = fragViewOffset;
        float param_6 = alpha;
        float param_7;
        float3 _447 = WaterSurface(param_5, param_6, param_7);
        alpha = param_6;
        float reflection = param_7;
        float3 glint = _447;
        color = lerp(color, camera_fogColor.xyz, (reflection * 0.60000002384185791015625f).xxx) + glint;
    }
    float3 param_8 = fragViewOffset;
    float3 foggedColor = lerp(color, camera_fogColor.xyz, FogFactor(param_8).xxx);
    outColor = float4(lerp(camera_fogColor.xyz, foggedColor, fragChunkFade.xxx), alpha);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragSkyLight = stage_input.fragSkyLight;
    fragUV = stage_input.fragUV;
    fragLayer = stage_input.fragLayer;
    fragBiomeTint = stage_input.fragBiomeTint;
    fragBlockLight = stage_input.fragBlockLight;
    fragFoliageHeight = stage_input.fragFoliageHeight;
    fragViewOffset = stage_input.fragViewOffset;
    fragChunkFade = stage_input.fragChunkFade;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
