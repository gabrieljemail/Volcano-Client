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
static float4 inPositionYaw;
static float4 inPartRotation;
static float3 inPosition;
static float4 inPartPivot;
static float4 inPartOffset;
static float3 inNormal;
static float4 inBodyPose;
static float3 fragNormal;
static float2 fragUV;
static float2 inUV;
static float4 fragHurt;
static float4 inHurtColor;
static float3 fragTint;
static float2 fragLight;

struct SPIRV_Cross_Input
{
    float3 inPosition : TEXCOORD0;
    float3 inNormal : TEXCOORD1;
    float2 inUV : TEXCOORD2;
    float4 inPositionYaw : TEXCOORD3;
    float4 inHurtColor : TEXCOORD4;
    float4 inPartPivot : TEXCOORD5;
    float4 inPartOffset : TEXCOORD6;
    float4 inPartRotation : TEXCOORD7;
    float4 inBodyPose : TEXCOORD8;
};

struct SPIRV_Cross_Output
{
    float3 fragNormal : TEXCOORD0;
    float2 fragUV : TEXCOORD1;
    float4 fragHurt : TEXCOORD2;
    nointerpolation float2 fragLight : TEXCOORD4;
    nointerpolation float3 fragTint : TEXCOORD5;
    float4 gl_Position : SV_Position;
};

float mod(float x, float y)
{
    return x - y * floor(x / y);
}

float2 mod(float2 x, float2 y)
{
    return x - y * floor(x / y);
}

float3 mod(float3 x, float3 y)
{
    return x - y * floor(x / y);
}

float4 mod(float4 x, float4 y)
{
    return x - y * floor(x / y);
}

void vert_main()
{
    float s = sin(inPositionYaw.w);
    float c = cos(inPositionYaw.w);
    float3x3 yawRotation = float3x3(float3(c, 0.0f, -s), float3(0.0f, 1.0f, 0.0f), float3(s, 0.0f, c));
    float sx = sin(inPartRotation.x);
    float cx = cos(inPartRotation.x);
    float sy = sin(inPartRotation.y);
    float cy = cos(inPartRotation.y);
    float sz = sin(inPartRotation.z);
    float cz = cos(inPartRotation.z);
    float3x3 rotX = float3x3(float3(1.0f, 0.0f, 0.0f), float3(0.0f, cx, sx), float3(0.0f, -sx, cx));
    float3x3 rotY = float3x3(float3(cy, 0.0f, -sy), float3(0.0f, 1.0f, 0.0f), float3(sy, 0.0f, cy));
    float3x3 rotZ = float3x3(float3(cz, sz, 0.0f), float3(-sz, cz, 0.0f), float3(0.0f, 0.0f, 1.0f));
    float3x3 partRotation = mul(rotX, mul(rotY, rotZ));
    float3 localPos = (mul(inPosition - inPartPivot.xyz, partRotation) + inPartPivot.xyz) + inPartOffset.xyz;
    float3 localNormal = mul(inNormal, partRotation);
    float sd = sin(inPartRotation.w);
    float cd = cos(inPartRotation.w);
    float3x3 deathRotation = float3x3(float3(cd, sd, 0.0f), float3(-sd, cd, 0.0f), float3(0.0f, 0.0f, 1.0f));
    localPos = mul(localPos, deathRotation);
    localNormal = mul(localNormal, deathRotation);
    float sp = sin(inBodyPose.x);
    float cp = cos(inBodyPose.x);
    float3x3 pitchRotation = float3x3(float3(1.0f, 0.0f, 0.0f), float3(0.0f, cp, sp), float3(0.0f, -sp, cp));
    localPos = mul(localPos, pitchRotation);
    localNormal = mul(localNormal, pitchRotation);
    float3 worldPos = mul(localPos, yawRotation) + inPositionYaw.xyz;
    gl_Position = mul(float4(worldPos, 1.0f), mul(camera_view, camera_proj));
    fragNormal = mul(localNormal, yawRotation);
    fragUV = inUV;
    fragHurt = inHurtColor;
    float sky = floor(inPartOffset.w / 16.0f);
    float packedTint = inPartPivot.w;
    float3 _232;
    if (packedTint > 0.0f)
    {
        _232 = float3(floor(packedTint / 65536.0f), mod(floor(packedTint / 256.0f), 256.0f), mod(packedTint, 256.0f)) / 255.0f.xxx;
    }
    else
    {
        _232 = 1.0f.xxx;
    }
    fragTint = _232;
    fragLight = float2(sky, inPartOffset.w - (sky * 16.0f)) / 15.0f.xx;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inPositionYaw = stage_input.inPositionYaw;
    inPartRotation = stage_input.inPartRotation;
    inPosition = stage_input.inPosition;
    inPartPivot = stage_input.inPartPivot;
    inPartOffset = stage_input.inPartOffset;
    inNormal = stage_input.inNormal;
    inBodyPose = stage_input.inBodyPose;
    inUV = stage_input.inUV;
    inHurtColor = stage_input.inHurtColor;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.fragNormal = fragNormal;
    stage_output.fragUV = fragUV;
    stage_output.fragHurt = fragHurt;
    stage_output.fragTint = fragTint;
    stage_output.fragLight = fragLight;
    return stage_output;
}
