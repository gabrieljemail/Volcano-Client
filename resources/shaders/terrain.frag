#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    float lightingEnabled;
    float brightnessFactor;
    float skyLightFactor;
    float lightCurveBias;
    float shadeTop;
    float shadeBottom;
    float shadeNorthSouth;
    float shadeEastWest;
} camera;

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in uint fragLayer;
layout(location = 2) in float fragSkyLight;
layout(location = 3) flat in uint fragBiomeTinted;
layout(location = 4) in float fragBlockLight;
layout(location = 5) in float fragFaceShade;

layout(location = 0) out vec4 outColor;

// Placeholder plains biome color (see BiomeColors.hpp) until per-block biome
// data is available from the server. Whether a face is tinted at all is
// still driven by the real per-texture whitelist, packed per-vertex.
const vec3 BIOME_COLOR_PLAINS = vec3(145.0 / 255.0, 189.0 / 255.0, 89.0 / 255.0);

// Bias-parameterized brightness curve (Graphics.LightCurveBias). Endpoints
// stay fixed regardless of bias; bias = 0 is linear, higher bows mid-range
// levels toward the ceiling. Set to 0 once real GI exists.
float LightCurve(float rawLevel01, float bias) {
    return 1.0 - (1.0 - rawLevel01) / (1.0 + bias * rawLevel01);
}

// Remaps LightCurve's [0,1] output to the final brightness multiplier.
const float BRIGHTNESS_FLOOR = 0.05;
const float BRIGHTNESS_CEILING = 1.0;

// Sky-only light caps below BRIGHTNESS_CEILING so a real light source still
// reads as glowing next to it.
const float AMBIENT_CEILING = 0.85;

// Vanilla's own "Bright" curve (see the real notGamma(vec3) in
// resources/minecraft/assets/minecraft/shaders/core/lightmap.fsh)
float NotGamma(float level) {
    float inverted = 1.0 - level;
    return 1.0 - (inverted * inverted * inverted * inverted);
}

void main() {
    vec4 texColor = texture(textureArray, vec3(fragUV, float(fragLayer)));
    vec3 tint = (fragBiomeTinted != 0u) ? BIOME_COLOR_PLAINS : vec3(1.0);

    // skyLightFactor scales the sky channel only — block light stays
    // constant day or night, matching a torch/glowstone.
    float skyBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(fragSkyLight, camera.lightCurveBias) * camera.skyLightFactor);
    float blockBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(fragBlockLight, camera.lightCurveBias));
    float ambient = min(skyBrightness, AMBIENT_CEILING);
    float light = max(ambient, blockBrightness);

    // Final = (1-γ)*moody + γ*bright — vanilla's gamma blend. Fullbright
    // skips this entirely rather than blending toward γ=0.
    light = camera.lightingEnabled > 0.5 ? mix(light, NotGamma(light), camera.brightnessFactor) : 1.0;

    // fragFaceShade is fully resolved by terrain.vert (see its own comment
    // for why the branch lives there and not here) — a plain multiply, no
    // branching in this stage at all.
    outColor = vec4(texColor.rgb * tint * light * fragFaceShade, texColor.a);
}
