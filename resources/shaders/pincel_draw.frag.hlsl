cbuffer PushConstants
{
    float4 pc_viewport : packoffset(c0);
    float4 pc_rect : packoffset(c1);
    float4 pc_borders : packoffset(c2);
    float4 pc_borderColor : packoffset(c3);
    float pc_radius : packoffset(c4);
    int pc_kind : packoffset(c4.y);
};

Texture2D<float4> pincelTexture : register(t0);
SamplerState _pincelTexture_sampler : register(s0);

static float4 gl_FragCoord;
static float4 fragColor;
static float4 outColor;
static float2 fragUV;

struct SPIRV_Cross_Input
{
    float2 fragUV : TEXCOORD0;
    float4 fragColor : TEXCOORD1;
    float4 gl_FragCoord : SV_Position;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

float RoundedDistance(float2 _point, float2 size, float cornerRadius)
{
    float2 q = abs(_point - (size * 0.5f)) - ((size * 0.5f) - cornerRadius.xx);
    return (length(max(q, 0.0f.xx)) + min(max(q.x, q.y), 0.0f)) - cornerRadius;
}

float Coverage(float _distance)
{
    float antialias = max(fwidth(_distance), 9.9999997473787516355514526367188e-05f);
    return 1.0f - smoothstep(-antialias, antialias, _distance);
}

void frag_main()
{
    if (pc_kind == 0)
    {
        float2 local = gl_FragCoord.xy - pc_rect.xy;
        float2 param = local;
        float2 param_1 = pc_rect.zw;
        float param_2 = max(pc_radius, 0.0f);
        float param_3 = RoundedDistance(param, param_1, param_2);
        float outer = Coverage(param_3);
        float2 inset = float2(pc_borders.w, pc_borders.x);
        float2 innerSize = pc_rect.zw - float2(pc_borders.y + pc_borders.w, pc_borders.x + pc_borders.z);
        float inner = 0.0f;
        bool _135 = innerSize.x > 0.0f;
        bool _141;
        if (_135)
        {
            _141 = innerSize.y > 0.0f;
        }
        else
        {
            _141 = _135;
        }
        if (_141)
        {
            float insetRadius = max(0.0f, pc_radius - max(max(pc_borders.x, pc_borders.y), max(pc_borders.z, pc_borders.w)));
            float2 param_4 = local - inset;
            float2 param_5 = innerSize;
            float param_6 = insetRadius;
            float param_7 = RoundedDistance(param_4, param_5, param_6);
            inner = Coverage(param_7);
        }
        float4 fillColor = float4(fragColor.xyz * fragColor.w, fragColor.w);
        float4 edgeColor = float4(pc_borderColor.xyz * pc_borderColor.w, pc_borderColor.w);
        outColor = (fillColor * inner) + (edgeColor * max(outer - inner, 0.0f));
    }
    else
    {
        if (pc_kind == 1)
        {
            float4 sampleColor = pincelTexture.Sample(_pincelTexture_sampler, fragUV);
            float _233;
            if (pc_radius > 0.0f)
            {
                float2 param_8 = gl_FragCoord.xy - pc_rect.xy;
                float2 param_9 = pc_rect.zw;
                float param_10 = pc_radius;
                float param_11 = RoundedDistance(param_8, param_9, param_10);
                _233 = Coverage(param_11);
            }
            else
            {
                _233 = 1.0f;
            }
            float mask = _233;
            float alpha = sampleColor.w * mask;
            outColor = float4(sampleColor.xyz * alpha, alpha);
        }
        else
        {
            float alpha_1 = pincelTexture.Sample(_pincelTexture_sampler, fragUV).x * fragColor.w;
            outColor = float4(fragColor.xyz * alpha_1, alpha_1);
        }
    }
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_FragCoord = stage_input.gl_FragCoord;
    gl_FragCoord.w = 1.0 / gl_FragCoord.w;
    fragColor = stage_input.fragColor;
    fragUV = stage_input.fragUV;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
