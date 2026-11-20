cbuffer CameraUBO : register(b0)
{
    row_major float4x4 camera_view : packoffset(c0);
    row_major float4x4 camera_proj : packoffset(c4);
};

cbuffer PushConstants
{
    float4 pc_worldMin : packoffset(c0);
    float4 pc_worldMax : packoffset(c1);
    float4 pc_color : packoffset(c2);
};


static float4 gl_Position;
static float3 inPosition;
static float4 fragColor;

struct SPIRV_Cross_Input
{
    float3 inPosition : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 fragColor : TEXCOORD0;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    float3 worldPos = lerp(pc_worldMin.xyz, pc_worldMax.xyz, inPosition);
    gl_Position = mul(float4(worldPos, 1.0f), mul(camera_view, camera_proj));
    fragColor = pc_color;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inPosition = stage_input.inPosition;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.fragColor = fragColor;
    return stage_output;
}
