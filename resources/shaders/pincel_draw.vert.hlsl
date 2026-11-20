cbuffer PushConstants
{
    float4 pc_viewport : packoffset(c0);
    float4 pc_rect : packoffset(c1);
    float4 pc_borders : packoffset(c2);
    float4 pc_borderColor : packoffset(c3);
    float pc_radius : packoffset(c4);
    int pc_kind : packoffset(c4.y);
};


static float4 gl_Position;
static float2 inPosition;
static float2 fragUV;
static float2 inUV;
static float4 fragColor;
static float4 inColor;

struct SPIRV_Cross_Input
{
    float2 inPosition : TEXCOORD0;
    float2 inUV : TEXCOORD1;
    float4 inColor : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float2 fragUV : TEXCOORD0;
    float4 fragColor : TEXCOORD1;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    float2 position = float2(((inPosition.x * 2.0f) / pc_viewport.x) - 1.0f, ((inPosition.y * 2.0f) / pc_viewport.y) - 1.0f);
    gl_Position = float4(position, 0.0f, 1.0f);
    fragUV = inUV;
    fragColor = inColor;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inPosition = stage_input.inPosition;
    inUV = stage_input.inUV;
    inColor = stage_input.inColor;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.fragUV = fragUV;
    stage_output.fragColor = fragColor;
    return stage_output;
}
