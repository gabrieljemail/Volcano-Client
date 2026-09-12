#version 450

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in uint fragLayer;
layout(location = 2) in float fragLight;
layout(location = 3) flat in uint fragBiomeTinted;

layout(location = 0) out vec4 outColor;

// Same placeholder biome tint as terrain.frag — see its own comment.
const vec3 BIOME_COLOR_PLAINS = vec3(145.0 / 255.0, 189.0 / 255.0, 89.0 / 255.0);

void main() {
    vec4 texColor = texture(textureArray, vec3(fragUV, float(fragLayer)));

    // Non-cube geometry is drawn with alpha blending and no depth write
    // (see VulkanInit's nonCubicPipeline) — discard fully transparent
    // texels outright so cross billboards' empty corners don't still write
    // (zero-alpha, but still real) fragments that could occlude geometry
    // behind them in a later draw within the same pass.
    if (texColor.a < 0.01) discard;

    vec3 tint = (fragBiomeTinted != 0u) ? BIOME_COLOR_PLAINS : vec3(1.0);
    outColor = vec4(texColor.rgb * tint * fragLight, texColor.a);
}
