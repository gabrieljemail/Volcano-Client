cbuffer Push
{
    row_major float4x4 push_mvp : packoffset(c0);
    float4 push_skyTop : packoffset(c4);
    float4 push_horizon : packoffset(c5);
    float4 push_sunDir : packoffset(c6);
};

Texture2DArray<float4> celestial : register(t1);
SamplerState _celestial_sampler : register(s1);
TextureCube<float4> panorama : register(t0);
SamplerState _panorama_sampler : register(s0);

static float4 outColor;
static float3 texCoord;

struct SPIRV_Cross_Input
{
    float3 texCoord : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

float4 Celestial(float3 dir, float3 axis, float layer)
{
    float facing = dot(dir, axis);
    if (facing <= 0.0f)
    {
        return 0.0f.xxxx;
    }
    float3 onPlane = (dir / facing.xxx) - axis;
    float3 across = float3(0.0f, 0.0f, 1.0f);
    float3 along = cross(across, axis);
    float2 uv = (float2(dot(onPlane, along), dot(onPlane, across)) / 0.300000011920928955078125f.xx) + 0.5f.xx;
    bool _63 = any(bool2(uv.x < 0.0f.xx.x, uv.y < 0.0f.xx.y));
    bool _71;
    if (!_63)
    {
        _71 = any(bool2(uv.x > 1.0f.xx.x, uv.y > 1.0f.xx.y));
    }
    else
    {
        _71 = _63;
    }
    if (_71)
    {
        return 0.0f.xxxx;
    }
    return celestial.Sample(_celestial_sampler, float3(uv, layer));
}

void frag_main()
{
    if (push_skyTop.w < 0.5f)
    {
        outColor = float4(panorama.Sample(_panorama_sampler, texCoord).xyz, 1.0f);
        return;
    }
    float3 dir = normalize(texCoord);
    float3 color = lerp(push_horizon.xyz, push_skyTop.xyz, smoothstep(0.0f, 0.5f, dir.y).xxx);
    float3 sun = normalize(push_sunDir.xyz);
    float sunNearHorizon = 1.0f - smoothstep(0.0f, 0.3499999940395355224609375f, abs(sun.y));
    float towardSun = pow(max(dot(normalize(float3(dir.x, 0.0f, dir.z) + 9.9999997473787516355514526367188e-06f.xxx), normalize(float3(sun.x, 0.0f, sun.z) + 9.9999997473787516355514526367188e-06f.xxx)), 0.0f), 3.0f);
    float nearHorizon = 1.0f - smoothstep(0.0f, 0.449999988079071044921875f, abs(dir.y));
    color = lerp(color, float3(1.0f, 0.2700000107288360595703125f, 0.0500000007450580596923828125f), (((sunNearHorizon * towardSun) * nearHorizon) * 0.800000011920928955078125f).xxx);
    float3 param = dir;
    float3 param_1 = sun;
    float param_2 = 0.0f;
    float4 sunTexel = Celestial(param, param_1, param_2);
    float3 param_3 = dir;
    float3 param_4 = -sun;
    float param_5 = 1.0f + push_horizon.w;
    float4 moonTexel = Celestial(param_3, param_4, param_5);
    color += ((sunTexel.xyz * sunTexel.w) + (moonTexel.xyz * moonTexel.w));
    outColor = float4(color, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    texCoord = stage_input.texCoord;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
