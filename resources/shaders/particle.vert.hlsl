static const float2 _19[6] = { (-1.0f).xx, float2(1.0f, -1.0f), 1.0f.xx, (-1.0f).xx, 1.0f.xx, float2(-1.0f, 1.0f) };

cbuffer Push
{
    row_major float4x4 push_viewProj : packoffset(c0);
    float4 push_right : packoffset(c4);
    float4 push_up : packoffset(c5);
};


static float4 gl_Position;
static int gl_VertexIndex;
static float2 inLayerRoll;
static float4 inPositionSize;
static float2 outUv;
static float4 inUv;
static float4 outColor;
static float4 inColor;
static float outLayer;

struct SPIRV_Cross_Input
{
    float4 inPositionSize : TEXCOORD0;
    float4 inColor : TEXCOORD1;
    float4 inUv : TEXCOORD2;
    float2 inLayerRoll : TEXCOORD3;
    uint gl_VertexIndex : SV_VertexID;
};

struct SPIRV_Cross_Output
{
    float2 outUv : TEXCOORD0;
    float4 outColor : TEXCOORD1;
    nointerpolation float outLayer : TEXCOORD2;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    float2 corner = _19[gl_VertexIndex];
    float s = sin(inLayerRoll.y);
    float c = cos(inLayerRoll.y);
    float2 rolled = float2((c * corner.x) - (s * corner.y), (s * corner.x) + (c * corner.y));
    float3 world = inPositionSize.xyz + (((push_right.xyz * rolled.x) + (push_up.xyz * rolled.y)) * (inPositionSize.w * 0.5f));
    gl_Position = mul(float4(world, 1.0f), push_viewProj);
    float2 t = (corner * 0.5f) + 0.5f.xx;
    outUv = float2(lerp(inUv.x, inUv.z, t.x), lerp(inUv.w, inUv.y, t.y));
    outColor = inColor;
    outLayer = inLayerRoll.x;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    inLayerRoll = stage_input.inLayerRoll;
    inPositionSize = stage_input.inPositionSize;
    inUv = stage_input.inUv;
    inColor = stage_input.inColor;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.outUv = outUv;
    stage_output.outColor = outColor;
    stage_output.outLayer = outLayer;
    return stage_output;
}
