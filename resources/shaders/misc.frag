#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    float lightingEnabled;
    float brightnessFactor;
    float skyLightFactor;
    float lightCurveBias;
} camera;

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in uint fragLayer;
layout(location = 2) in float fragSkyLight;
layout(location = 3) flat in uint fragBiomeTint;
layout(location = 4) in float fragBlockLight;

layout(location = 0) out vec4 outColor;

// Same placeholder biome tint as terrain.frag — see its own comment — plus a
// second placeholder for water (BiomeColors.hpp's BiomeTint::Water): water's
// own texture is grayscale and relies entirely on this multiply to read as
// blue, same reason grass/foliage need BIOME_COLOR_PLAINS.
const vec3 BIOME_COLOR_PLAINS = vec3(145.0 / 255.0, 189.0 / 255.0, 89.0 / 255.0);
const vec3 BIOME_COLOR_WATER = vec3(0x3F / 255.0, 0x76 / 255.0, 0xE4 / 255.0);

// Same curve/constants as terrain.frag — duplicated, no GLSL include mechanism.
float LightCurve(float rawLevel01, float bias) {
    return 1.0 - (1.0 - rawLevel01) / (1.0 + bias * rawLevel01);
}

const float BRIGHTNESS_FLOOR = 0.05;
const float BRIGHTNESS_CEILING = 1.0;
const float AMBIENT_CEILING = 0.85;

// Same as terrain.frag's NotGamma.
float NotGamma(float level) {
    float inverted = 1.0 - level;
    return 1.0 - (inverted * inverted * inverted * inverted);
}

void main() {
    vec4 texColor = texture(textureArray, vec3(fragUV, float(fragLayer)));

    // Shared by both non-cube passes (VulkanInit's nonCubicPipeline cutout
    // pass and translucentPipeline) — discard fully transparent texels
    // outright so cross billboards' empty corners (cutout pass) don't still
    // write a depth value, and so translucent geometry's own fully-empty
    // texels don't write color either. Harmless for water, whose texture has
    // no near-zero-alpha texels to begin with.
    if (texColor.a < 0.01) discard;

    vec3 tint = fragBiomeTint == 1u ? BIOME_COLOR_PLAINS : (fragBiomeTint == 2u ? BIOME_COLOR_WATER : vec3(1.0));

    // Same skyLightFactor/gamma treatment as terrain.frag.
    float skyBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(fragSkyLight, camera.lightCurveBias) * camera.skyLightFactor);
    float blockBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(fragBlockLight, camera.lightCurveBias));
    float ambient = min(skyBrightness, AMBIENT_CEILING);
    float light = max(ambient, blockBrightness);
    light = camera.lightingEnabled > 0.5 ? mix(light, NotGamma(light), camera.brightnessFactor) : 1.0;

    outColor = vec4(texColor.rgb * tint * light, texColor.a);
}
