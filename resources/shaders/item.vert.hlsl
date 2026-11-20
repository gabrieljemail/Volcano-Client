cbuffer PushConstants
{
    row_major float4x4 pc_mvp : packoffset(c0);
    float4 pc_normalRow0 : packoffset(c4);
    float4 pc_normalRow1 : packoffset(c5);
    float4 pc_normalRow2 : packoffset(c6);
    float4 pc_params : packoffset(c7);
};


static float4 gl_Position;
static float3 inPosition;
static float3 inNormal;
static float2 fragUV;
static float2 inUV;
static float fragLayer;
static float inLayer;
static float fragTime;
static float fragLight;

struct SPIRV_Cross_Input
{
    float3 inPosition : TEXCOORD0;
    float3 inNormal : TEXCOORD1;
    float2 inUV : TEXCOORD2;
    float inLayer : TEXCOORD3;
};

struct SPIRV_Cross_Output
{
    float2 fragUV : TEXCOORD0;
    float fragLayer : TEXCOORD1;
    float fragLight : TEXCOORD2;
    float fragTime : TEXCOORD3;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    gl_Position = mul(float4(inPosition, 1.0f), pc_mvp);
    float3 normal = normalize(float3(dot(pc_normalRow0.xyz, inNormal), dot(pc_normalRow1.xyz, inNormal), dot(pc_normalRow2.xyz, inNormal)));
    float lit = max(dot(normal, float3(0.15690000355243682861328125f, 0.78450000286102294921875f, -0.549199998378753662109375f)), 0.0f) + max(dot(normal, float3(-0.15690000355243682861328125f, 0.78450000286102294921875f, 0.549199998378753662109375f)), 0.0f);
    fragUV = inUV;
    fragLayer = inLayer;
    fragTime = pc_params.z;
    float _100;
    if (pc_params.x > 0.5f)
    {
        _100 = 1.0f;
    }
    else
    {
        _100 = min(1.0f, 0.4000000059604644775390625f + (0.60000002384185791015625f * lit));
    }
    fragLight = _100 * (1.0f - pc_params.y);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inPosition = stage_input.inPosition;
    inNormal = stage_input.inNormal;
    inUV = stage_input.inUV;
    inLayer = stage_input.inLayer;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.fragUV = fragUV;
    stage_output.fragLayer = fragLayer;
    stage_output.fragTime = fragTime;
    stage_output.fragLight = fragLight;
    return stage_output;
}
