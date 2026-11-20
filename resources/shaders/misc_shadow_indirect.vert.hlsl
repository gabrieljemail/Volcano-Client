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


static float4 gl_Position;
static uint fragLayer;
static uint inPacked;
static uint fragBiomeTint;
static float3 inPosition;
static float4 sectionData;
static float3 fragViewOffset;
static float2 fragUV;
static float2 inUV;
static float fragSkyLight;
static float fragBlockLight;
static float fragChunkFade;
static float fragFoliageHeight;

struct SPIRV_Cross_Input
{
    float3 inPosition : TEXCOORD0;
    float2 inUV : TEXCOORD1;
    uint inPacked : TEXCOORD2;
    float4 sectionData : TEXCOORD3;
};

struct SPIRV_Cross_Output
{
    float2 fragUV : TEXCOORD0;
    nointerpolation uint fragLayer : TEXCOORD1;
    float fragSkyLight : TEXCOORD2;
    nointerpolation uint fragBiomeTint : TEXCOORD3;
    float fragBlockLight : TEXCOORD4;
    float3 fragViewOffset : TEXCOORD5;
    float fragChunkFade : TEXCOORD6;
    float fragFoliageHeight : TEXCOORD7;
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

void vert_main()
{
    fragLayer = spvBitfieldUExtract(inPacked, 0, 16);
    uint blockLight = spvBitfieldUExtract(inPacked, 16, 4);
    uint skyLight = spvBitfieldUExtract(inPacked, 20, 4);
    fragBiomeTint = spvBitfieldUExtract(inPacked, 24, 2);
    float4 worldPos = float4(inPosition + sectionData.xyz, 1.0f);
    gl_Position = mul(worldPos, mul(camera_view, camera_proj));
    fragViewOffset = worldPos.xyz - camera_cameraPosition.xyz;
    fragUV = inUV;
    fragSkyLight = float(skyLight) / 15.0f;
    fragBlockLight = float(blockLight) / 15.0f;
    fragChunkFade = sectionData.w;
    fragFoliageHeight = clamp(1.0f - inUV.y, 0.0f, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inPacked = stage_input.inPacked;
    inPosition = stage_input.inPosition;
    sectionData = stage_input.sectionData;
    inUV = stage_input.inUV;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.fragLayer = fragLayer;
    stage_output.fragBiomeTint = fragBiomeTint;
    stage_output.fragViewOffset = fragViewOffset;
    stage_output.fragUV = fragUV;
    stage_output.fragSkyLight = fragSkyLight;
    stage_output.fragBlockLight = fragBlockLight;
    stage_output.fragChunkFade = fragChunkFade;
    stage_output.fragFoliageHeight = fragFoliageHeight;
    return stage_output;
}
