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

Texture2D<float4> armorTexture : register(t0);
SamplerState _armorTexture_sampler : register(s0);
Texture2D<float4> glintTexture : register(t0);
SamplerState _glintTexture_sampler : register(s0);

static float2 fragUV;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float2 fragUV : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    if (armorTexture.Sample(_armorTexture_sampler, fragUV).w < 0.5f)
    {
        discard;
    }
    float2 scaled = fragUV * 0.1599999964237213134765625f;
    float2 tilted = float2((0.9848077297210693359375f * scaled.x) - (0.173648178577423095703125f * scaled.y), (0.173648178577423095703125f * scaled.x) + (0.9848077297210693359375f * scaled.y));
    float2 uv = tilted + float2(-frac(camera_time / 27.5f), frac(camera_time / 7.5f));
    outColor = float4(glintTexture.Sample(_glintTexture_sampler, uv).xyz * 0.75f, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragUV = stage_input.fragUV;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
