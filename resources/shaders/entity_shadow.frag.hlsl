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

static float3 fragNormal;
static float3 fragViewOffset;
static float4 fragColor;
static float4 fragHurt;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float3 fragNormal : TEXCOORD0;
    float4 fragColor : TEXCOORD1;
    float4 fragHurt : TEXCOORD2;
    float3 fragViewOffset : TEXCOORD3;
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
    float4 _150 = float4(uv + float2(-spread.x, -spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _150.xyz, _150.w);
    float4 _169 = float4(uv + float2(spread.x, -spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _169.xyz, _169.w);
    float4 _188 = float4(uv + float2(-spread.x, spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _188.xyz, _188.w);
    float4 _206 = float4(uv + float2(spread.x, spread.y), float(cascade), depth);
    lit += shadowMap.SampleCmp(_shadowMap_sampler, _206.xyz, _206.w);
    lit *= 0.25f;
    float inside = ((((step(0.0f, uv.x) * step(uv.x, 1.0f)) * step(0.0f, uv.y)) * step(uv.y, 1.0f)) * step(clip.z, 1.0f)) * (1.0f - step(camera_shadowSplits.z, _distance));
    return lerp(1.0f, lit, inside);
}

void frag_main()
{
    float diffuse = max(dot(normalize(fragNormal), float3(0.3837999999523162841796875f, 0.78450000286102294921875f, 0.48849999904632568359375f)), 0.0f);
    float sunFacing = max(dot(normalize(fragNormal), camera_sunDirection.xyz), 0.0f);
    float3 param = fragViewOffset;
    float3 param_1 = normalize(fragNormal);
    float param_2 = sunFacing;
    diffuse *= lerp(1.0f - camera_shadowParams.x, 1.0f, ShadowFactor(param, param_1, param_2));
    float brightness = 0.4000000059604644775390625f + (0.60000002384185791015625f * diffuse);
    float3 shaded = fragColor.xyz * brightness;
    float3 finalColor = lerp(shaded, fragHurt.xyz, fragHurt.w.xxx);
    outColor = float4(finalColor, fragColor.w);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragNormal = stage_input.fragNormal;
    fragViewOffset = stage_input.fragViewOffset;
    fragColor = stage_input.fragColor;
    fragHurt = stage_input.fragHurt;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
