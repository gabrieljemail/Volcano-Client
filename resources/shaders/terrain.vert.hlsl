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

cbuffer PushConstants
{
    row_major float4x4 pc_model : packoffset(c0);
    float pc_fade : packoffset(c4);
};


static float4 gl_Position;
static uint3 packedData;
static uint fragBiomeTinted;
static float fragFaceShade;
static float4 fragSpecular;
static float fragAOMultiplier;
static float3 fragViewOffset;
static float2 fragUV;
static uint fragLayer;
static float fragSkyLight;
static float fragBlockLight;
static float fragChunkFade;

struct SPIRV_Cross_Input
{
    uint3 packedData : TEXCOORD0;
};

struct SPIRV_Cross_Output
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
    float4 gl_Position : SV_Position;
};

uint spvBitfieldUExtract(uint Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

uint2 spvBitfieldUExtract(uint2 Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

uint3 spvBitfieldUExtract(uint3 Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

uint4 spvBitfieldUExtract(uint4 Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

int spvBitfieldSExtract(int Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

int2 spvBitfieldSExtract(int2 Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int2 Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

int3 spvBitfieldSExtract(int3 Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int3 Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

int4 spvBitfieldSExtract(int4 Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int4 Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

float FaceShade(uint normalIndex)
{
    if (normalIndex == 0u)
    {
        return camera_shadeTop;
    }
    if (normalIndex == 1u)
    {
        return camera_shadeBottom;
    }
    if ((normalIndex == 2u) || (normalIndex == 3u))
    {
        return camera_shadeNorthSouth;
    }
    return camera_shadeEastWest;
}

float3 FaceNormal(uint normalIndex)
{
    if (normalIndex == 0u)
    {
        return float3(0.0f, 1.0f, 0.0f);
    }
    if (normalIndex == 1u)
    {
        return float3(0.0f, -1.0f, 0.0f);
    }
    if (normalIndex == 2u)
    {
        return float3(0.0f, 0.0f, -1.0f);
    }
    if (normalIndex == 3u)
    {
        return float3(0.0f, 0.0f, 1.0f);
    }
    if (normalIndex == 4u)
    {
        return float3(1.0f, 0.0f, 0.0f);
    }
    return float3(-1.0f, 0.0f, 0.0f);
}

void vert_main()
{
    uint word0 = packedData.x;
    uint word1 = packedData.y;
    uint word2 = packedData.z;
    float x = float(spvBitfieldUExtract(word0, 0, 6));
    float z = float(spvBitfieldUExtract(word0, 6, 6));
    float y = float(spvBitfieldUExtract(word0, 12, 9));
    fragBiomeTinted = spvBitfieldUExtract(word0, 26, 3);
    uint normalIndex = spvBitfieldUExtract(word0, 21, 3);
    uint param = normalIndex;
    fragFaceShade = FaceShade(param);
    uint param_1 = normalIndex;
    fragSpecular = float4(FaceNormal(param_1), float(spvBitfieldUExtract(word1, 24, 3)) / 7.0f);
    uint blockLight = spvBitfieldUExtract(word1, 16, 4);
    uint skyLight = spvBitfieldUExtract(word1, 20, 4);
    uint textureLayer = spvBitfieldUExtract(word1, 0, 16);
    uint ao = spvBitfieldUExtract(word0, 24, 2);
    fragAOMultiplier = lerp(1.0f, 0.20000000298023223876953125f, float(ao) / 3.0f);
    float u = float(spvBitfieldUExtract(word2, 0, 8));
    float v = float(spvBitfieldUExtract(word2, 8, 8));
    float3 localPos = float3(x, y, z);
    float4 worldPos = mul(float4(localPos, 1.0f), pc_model);
    gl_Position = mul(worldPos, mul(camera_view, camera_proj));
    fragViewOffset = worldPos.xyz - camera_cameraPosition.xyz;
    fragUV = float2(u, v);
    fragLayer = textureLayer;
    fragSkyLight = float(skyLight) / 15.0f;
    fragBlockLight = float(blockLight) / 15.0f;
    fragChunkFade = pc_fade;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    packedData = stage_input.packedData;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.fragBiomeTinted = fragBiomeTinted;
    stage_output.fragFaceShade = fragFaceShade;
    stage_output.fragSpecular = fragSpecular;
    stage_output.fragAOMultiplier = fragAOMultiplier;
    stage_output.fragViewOffset = fragViewOffset;
    stage_output.fragUV = fragUV;
    stage_output.fragLayer = fragLayer;
    stage_output.fragSkyLight = fragSkyLight;
    stage_output.fragBlockLight = fragBlockLight;
    stage_output.fragChunkFade = fragChunkFade;
    return stage_output;
}
