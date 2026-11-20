struct EntityInstance
{
    float4 positionYaw;
    float4 halfExtents;
    float4 color;
    float4 hurtColor;
};

ByteAddressBuffer _15 : register(t0);
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


static float4 gl_Position;
static int gl_InstanceIndex;
static float3 inPosition;
static float3 fragViewOffset;
static float3 fragNormal;
static float3 inNormal;
static float4 fragColor;
static float4 fragHurt;

struct SPIRV_Cross_Input
{
    float3 inPosition : TEXCOORD0;
    float3 inNormal : TEXCOORD1;
    uint gl_InstanceIndex : SV_InstanceID;
};

struct SPIRV_Cross_Output
{
    float3 fragNormal : TEXCOORD0;
    float4 fragColor : TEXCOORD1;
    float4 fragHurt : TEXCOORD2;
    float3 fragViewOffset : TEXCOORD3;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    EntityInstance _23;
    _23.positionYaw = asfloat(_15.Load4(gl_InstanceIndex * 64 + 0));
    _23.halfExtents = asfloat(_15.Load4(gl_InstanceIndex * 64 + 16));
    _23.color = asfloat(_15.Load4(gl_InstanceIndex * 64 + 32));
    _23.hurtColor = asfloat(_15.Load4(gl_InstanceIndex * 64 + 48));
    EntityInstance inst;
    inst.positionYaw = _23.positionYaw;
    inst.halfExtents = _23.halfExtents;
    inst.color = _23.color;
    inst.hurtColor = _23.hurtColor;
    float3 halfExtents = inst.halfExtents.xyz;
    float yaw = inst.positionYaw.w;
    float s = sin(yaw);
    float c = cos(yaw);
    float3x3 yawRotation = float3x3(float3(c, 0.0f, -s), float3(0.0f, 1.0f, 0.0f), float3(s, 0.0f, c));
    float3 localPos = inPosition * (halfExtents * 2.0f);
    float3 rotatedPos = mul(localPos, yawRotation);
    float3 worldPos = (rotatedPos + inst.positionYaw.xyz) + float3(0.0f, halfExtents.y, 0.0f);
    gl_Position = mul(float4(worldPos, 1.0f), mul(camera_view, camera_proj));
    fragViewOffset = worldPos - camera_cameraPosition.xyz;
    fragNormal = mul(inNormal, yawRotation);
    fragColor = inst.color;
    fragHurt = inst.hurtColor;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_InstanceIndex = int(stage_input.gl_InstanceIndex);
    inPosition = stage_input.inPosition;
    inNormal = stage_input.inNormal;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.fragViewOffset = fragViewOffset;
    stage_output.fragNormal = fragNormal;
    stage_output.fragColor = fragColor;
    stage_output.fragHurt = fragHurt;
    return stage_output;
}
