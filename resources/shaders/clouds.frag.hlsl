cbuffer Push
{
    row_major float4x4 push_mvp : packoffset(c0);
    float4 push_color : packoffset(c4);
    float4 push_offset : packoffset(c5);
};


static float fragDistance;
static float4 outColor;
static float fragShade;

struct SPIRV_Cross_Input
{
    float fragShade : TEXCOORD0;
    float fragDistance : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    float fade = 1.0f - smoothstep(push_offset.w * 0.75f, push_offset.w, fragDistance);
    outColor = float4(push_color.xyz * fragShade, push_color.w * fade);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragDistance = stage_input.fragDistance;
    fragShade = stage_input.fragShade;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
