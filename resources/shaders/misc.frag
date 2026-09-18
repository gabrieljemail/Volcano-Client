#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    float lightingEnabled;
} camera;

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in uint fragLayer;
layout(location = 2) in float fragSkyLight;
layout(location = 3) flat in uint fragBiomeTinted;
layout(location = 4) in float fragBlockLight;

layout(location = 0) out vec4 outColor;

// Same placeholder biome tint as terrain.frag — see its own comment.
const vec3 BIOME_COLOR_PLAINS = vec3(145.0 / 255.0, 189.0 / 255.0, 89.0 / 255.0);

// Same brightness curve and bloom-style ambient cap as terrain.frag — see
// its own comments for the reasoning. Duplicated rather than shared since
// these are standalone GLSL files with no include mechanism wired up (same
// convention BIOME_COLOR_PLAINS above already follows).
float LightCurve(float rawLevel01) {
    return rawLevel01 / (4.0 - 3.0 * rawLevel01);
}

const float BRIGHTNESS_FLOOR = 0.05;
const float BRIGHTNESS_CEILING = 1.0;
const float AMBIENT_CEILING = 0.85;

void main() {
    vec4 texColor = texture(textureArray, vec3(fragUV, float(fragLayer)));

    // Non-cube geometry is drawn with alpha blending and depth write ON (see
    // VulkanInit's nonCubicPipeline's own comment on why this pass is really
    // a cutout pass, not a translucent one) — discard fully transparent
    // texels outright so cross billboards' empty corners don't still write
    // a depth value (or occlude translucent geometry behind them, once a
    // separate translucent pass exists).
    if (texColor.a < 0.01) discard;

    vec3 tint = (fragBiomeTinted != 0u) ? BIOME_COLOR_PLAINS : vec3(1.0);

    float skyBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(fragSkyLight));
    float blockBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(fragBlockLight));
    float ambient = min(skyBrightness, AMBIENT_CEILING);
    float light = camera.lightingEnabled > 0.5 ? max(ambient, blockBrightness) : 1.0;

    outColor = vec4(texColor.rgb * tint * light, texColor.a);
}
