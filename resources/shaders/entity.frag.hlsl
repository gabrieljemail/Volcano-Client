static float3 fragNormal;
static float4 fragColor;
static float4 fragHurt;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float3 fragNormal : TEXCOORD0;
    float4 fragColor : TEXCOORD1;
    float4 fragHurt : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    float diffuse = max(dot(normalize(fragNormal), float3(0.3837999999523162841796875f, 0.78450000286102294921875f, 0.48849999904632568359375f)), 0.0f);
    float brightness = 0.4000000059604644775390625f + (0.60000002384185791015625f * diffuse);
    float3 shaded = fragColor.xyz * brightness;
    float3 finalColor = lerp(shaded, fragHurt.xyz, fragHurt.w.xxx);
    outColor = float4(finalColor, fragColor.w);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    fragNormal = stage_input.fragNormal;
    fragColor = stage_input.fragColor;
    fragHurt = stage_input.fragHurt;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
