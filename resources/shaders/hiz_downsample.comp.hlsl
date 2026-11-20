static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

RWTexture2D<float> dstMip : register(u1);
RWTexture2D<float> srcMip : register(u0);

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
    int2 dstCoord = int2(gl_GlobalInvocationID.xy);
    uint _24_dummy_parameter;
    int2 dstSize = int2(spvImageSize(dstMip, _24_dummy_parameter));
    bool _32 = dstCoord.x >= dstSize.x;
    bool _42;
    if (!_32)
    {
        _42 = dstCoord.y >= dstSize.y;
    }
    else
    {
        _42 = _32;
    }
    if (_42)
    {
        return;
    }
    uint _49_dummy_parameter;
    int2 srcSize = int2(spvImageSize(srcMip, _49_dummy_parameter));
    int2 srcStart = dstCoord * int2(2, 2);
    int2 srcEnd = min((srcStart + int2(1, 1)), (srcSize - int2(1, 1)));
    if (dstCoord.x == (dstSize.x - 1))
    {
        srcEnd.x = srcSize.x - 1;
    }
    if (dstCoord.y == (dstSize.y - 1))
    {
        srcEnd.y = srcSize.y - 1;
    }
    float maxDepth = 0.0f;
    for (int y = srcStart.y; y <= srcEnd.y; y++)
    {
        for (int x = srcStart.x; x <= srcEnd.x; x++)
        {
            maxDepth = max(maxDepth, srcMip[int2(x, y)].xxxx.x);
        }
    }
    dstMip[dstCoord] = float4(maxDepth, 0.0f, 0.0f, 0.0f).x;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
