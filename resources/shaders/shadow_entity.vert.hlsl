cbuffer Push
{
    row_major float4x4 pc_lightViewProj : packoffset(c0);
};


static float4 gl_Position;
static float4x4 inModel;
static float3 inPosition;

struct SPIRV_Cross_Input
{
    float3 inPosition : TEXCOORD0;
    float4 inModel_0 : TEXCOORD1_0;
    float4 inModel_1 : TEXCOORD1_1;
    float4 inModel_2 : TEXCOORD1_2;
    float4 inModel_3 : TEXCOORD1_3;
};

struct SPIRV_Cross_Output
{
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    gl_Position = mul(mul(float4(inPosition, 1.0f), inModel), pc_lightViewProj);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inModel[0] = stage_input.inModel_0;
    inModel[1] = stage_input.inModel_1;
    inModel[2] = stage_input.inModel_2;
    inModel[3] = stage_input.inModel_3;
    inPosition = stage_input.inPosition;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    return stage_output;
}
