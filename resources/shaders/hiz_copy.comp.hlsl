static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

RWTexture2D<float> dstMip0 : register(u1);
Texture2D<float4> srcDepth : register(t0);
SamplerState _srcDepth_sampler : register(s0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint2 spvImageSize(RWTexture2D<float> Tex, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(ret.x, ret.y);
    Param = 0u;
    return ret;
}

void comp_main()
{
    int2 coord = int2(gl_GlobalInvocationID.xy);
    uint _24_dummy_parameter;
    int2 size = int2(spvImageSize(dstMip0, _24_dummy_parameter));
    bool _32 = coord.x >= size.x;
    bool _42;
    if (!_32)
    {
        _42 = coord.y >= size.y;
    }
    else
    {
        _42 = _32;
    }
    if (_42)
    {
        return;
    }
    float depth = srcDepth.Load(int3(coord, 0)).x;
    dstMip0[coord] = float4(depth, 0.0f, 0.0f, 0.0f).x;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
