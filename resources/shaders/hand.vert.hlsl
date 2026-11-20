cbuffer PushConstants
{
    row_major float4x4 pc_mvp : packoffset(c0);
    float4 pc_shade : packoffset(c4);
};


static float4 gl_Position;
static float3 inPosition;
static float2 fragUV;
static float2 inUV;
static float3 inNormal;
static float fragLight;

struct SPIRV_Cross_Input
{
    float3 inPosition : TEXCOORD0;
    float3 inNormal : TEXCOORD1;
    float2 inUV : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float2 fragUV : TEXCOORD0;
    float fragLight : TEXCOORD1;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    gl_Position = mul(float4(inPosition, 1.0f), pc_mvp);
    fragUV = inUV;
    float3 n = abs(inNormal);
    float _54;
    if (n.y > 0.5f)
    {
        _54 = (inNormal.y < 0.0f) ? 1.0f : 0.5f;
    }
    else
    {
        _54 = (n.z > 0.5f) ? 0.800000011920928955078125f : 0.60000002384185791015625f;
    }
    fragLight = _54;
    fragLight *= pc_shade.x;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inPosition = stage_input.inPosition;
    inUV = stage_input.inUV;
    inNormal = stage_input.inNormal;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.fragUV = fragUV;
    stage_output.fragLight = fragLight;
    return stage_output;
}
