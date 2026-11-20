static float4 outColor;
static float4 fragColor;

struct SPIRV_Cross_Input
{
    float4 fragColor : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    outColor = fragColor;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragColor = stage_input.fragColor;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
