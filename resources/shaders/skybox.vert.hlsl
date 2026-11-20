cbuffer Push
{
    row_major float4x4 push_mvp : packoffset(c0);
};


static float4 gl_Position;
static int gl_VertexIndex;
static float3 texCoord;

struct SPIRV_Cross_Input
{
    uint gl_VertexIndex : SV_VertexID;
};

struct SPIRV_Cross_Output
{
    float3 texCoord : TEXCOORD0;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    int face = gl_VertexIndex / 6;
    int k = gl_VertexIndex % 6;
    int _22;
    if (k < 3)
    {
        _22 = k;
    }
    else
    {
        int _29;
        if (k == 3)
        {
            _29 = 0;
        }
        else
        {
            _29 = k - 2;
        }
        _22 = _29;
    }
    int corner = _22;
    float2 uv = float2(((corner == 1) || (corner == 2)) ? 1.0f : (-1.0f), (corner >= 2) ? 1.0f : (-1.0f));
    float s = ((face & 1) == 0) ? 1.0f : (-1.0f);
    int axis = face >> 1;
    float3 _70;
    if (axis == 0)
    {
        _70 = float3(s, uv.x, uv.y);
    }
    else
    {
        float3 _85;
        if (axis == 1)
        {
            _85 = float3(uv.x, s, uv.y);
        }
        else
        {
            _85 = float3(uv.x, uv.y, s);
        }
        _70 = _85;
    }
    float3 pos = _70;
    texCoord = pos;
    gl_Position = mul(float4(pos, 1.0f), push_mvp).xyww;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.texCoord = texCoord;
    return stage_output;
}
