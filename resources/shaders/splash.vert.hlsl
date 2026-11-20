static const float2 _19[4] = { (-1.0f).xx, float2(1.0f, -1.0f), float2(-1.0f, 1.0f), 1.0f.xx };
static const float2 _25[4] = { 0.0f.xx, float2(1.0f, 0.0f), float2(0.0f, 1.0f), 1.0f.xx };

cbuffer PushConstants
{
    float2 pc_uvScale : packoffset(c0);
};


static float4 gl_Position;
static int gl_VertexIndex;
static float2 fragUV;

struct SPIRV_Cross_Input
{
    uint gl_VertexIndex : SV_VertexID;
};

struct SPIRV_Cross_Output
{
    float2 fragUV : TEXCOORD0;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    gl_Position = float4(_19[gl_VertexIndex], 0.0f, 1.0f);
    float2 uvOffset = (1.0f.xx - pc_uvScale) * 0.5f;
    fragUV = (_25[gl_VertexIndex] * pc_uvScale) + uvOffset;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.fragUV = fragUV;
    return stage_output;
}
