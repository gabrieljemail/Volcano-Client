cbuffer Push
{
    row_major float4x4 push_mvp : packoffset(c0);
    float4 push_color : packoffset(c4);
    float4 push_offset : packoffset(c5);
};


static float4 gl_Position;
static float3 inPosition;
static float fragShade;
static float inShade;
static float fragDistance;

struct SPIRV_Cross_Input
{
    float3 inPosition : TEXCOORD0;
    float inShade : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float fragShade : TEXCOORD0;
    float fragDistance : TEXCOORD1;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    gl_Position = mul(float4(inPosition, 1.0f), push_mvp);
    fragShade = inShade;
    fragDistance = length((inPosition + push_offset.xyz).xz);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inPosition = stage_input.inPosition;
    inShade = stage_input.inShade;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.fragShade = fragShade;
    stage_output.fragDistance = fragDistance;
    return stage_output;
}
