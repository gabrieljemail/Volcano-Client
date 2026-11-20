cbuffer CameraUBO : register(b0)
{
    row_major float4x4 camera_view : packoffset(c0);
    row_major float4x4 camera_proj : packoffset(c4);
};

cbuffer PushConstants
{
    float4 pc_worldMin : packoffset(c0);
    float4 pc_worldMax : packoffset(c1);
};


static float4 gl_Position;
static float3 inPosition;
static float2 fragUV;
static float2 inUV;

struct SPIRV_Cross_Input
{
    float3 inPosition : TEXCOORD0;
    float2 inUV : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float2 fragUV : TEXCOORD0;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    float3 worldPos = lerp(pc_worldMin.xyz, pc_worldMax.xyz, inPosition);
    gl_Position = mul(float4(worldPos, 1.0f), mul(camera_view, camera_proj));
    fragUV = inUV;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inPosition = stage_input.inPosition;
    inUV = stage_input.inUV;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.fragUV = fragUV;
    return stage_output;
}
