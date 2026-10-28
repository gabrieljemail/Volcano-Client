#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    float lightingEnabled;
    float brightnessFactor;
    float skyLightFactor;
    float lightFalloff;
    float shadeTop;
    float shadeBottom;
    float shadeNorthSouth;
    float shadeEastWest;
    vec4 fogColor;
    vec4 cameraPosition;
    float fogStart;
    float fogEnd;
    float specularStrength;
    float time;
    vec4 sunDirection;
    mat4 shadowViewProj[3];
    vec4 shadowParams; // x darkness, y texel (uv), w debug view
    vec4 shadowSplits; // far view distance of each cascade
    vec4 shadowTexels; // blocks per texel of each cascade
    vec4 shadowRanges; // depth span of each cascade, in blocks
} camera;

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in uint fragLayer;
layout(location = 2) in float fragSkyLight;
layout(location = 3) flat in uint fragBiomeTinted;
layout(location = 4) in float fragBlockLight;
layout(location = 5) in float fragAOMultiplier;
layout(location = 6) in float fragFaceShade;
layout(location = 7) in vec3 fragViewOffset;
layout(location = 8) in float fragChunkFade;
layout(location = 9) flat in vec4 fragSpecular;
#ifdef SHADOWS
layout(set = 2, binding = 0) uniform sampler2DArrayShadow shadowMap;

// 1 = lit, 0 = shadowed. Picks the tightest cascade covering this pixel's view
// depth, pushes the lookup off the surface along `normal`, and averages a
// 3x3 PCF. `facing` (how squarely the surface meets the sun, 0 = edge-on)
// scales the bias. Beyond the last cascade, or off a cascade's map, is lit.
float ShadowFactor(vec3 viewOffset, vec3 normal, float facing) {
    // Depth along the camera's view direction: what the cascades were sliced by.
    float distance = -dot(vec3(camera.view[0][2], camera.view[1][2], camera.view[2][2]), viewOffset);
    int cascade = int(step(camera.shadowSplits.x, distance) + step(camera.shadowSplits.y, distance));

    float texelWorld = camera.shadowTexels[cascade];
    vec3 world = viewOffset + camera.cameraPosition.xyz + normal * (texelWorld * 1.2);
    vec4 clip = camera.shadowViewProj[cascade] * vec4(world, 1.0);
    vec2 uv = clip.xy * 0.5 + 0.5;

    // Blocks of depth bias: a fraction of a texel, more as the surface turns edge-on.
    float biasBlocks = texelWorld * 0.7 * (1.0 + 2.0 * (1.0 - facing)) + 0.03;
    float depth = clip.z - biasBlocks / camera.shadowRanges[cascade];

    float texel = camera.shadowParams.y;
    float lit = 0.0;
    // Four taps a quarter texel out; each is already a bilinear 2x2 compare, so the
    // blur is about half the width (and under half the cost) of a 3x3 grid.
    vec2 spread = vec2(0.25) * texel;
    lit += texture(shadowMap, vec4(uv + vec2(-spread.x, -spread.y), float(cascade), depth));
    lit += texture(shadowMap, vec4(uv + vec2( spread.x, -spread.y), float(cascade), depth));
    lit += texture(shadowMap, vec4(uv + vec2(-spread.x,  spread.y), float(cascade), depth));
    lit += texture(shadowMap, vec4(uv + vec2( spread.x,  spread.y), float(cascade), depth));
    lit *= 0.25;

    float inside = step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0)
        * step(clip.z, 1.0) * (1.0 - step(camera.shadowSplits.z, distance));
    return mix(1.0, lit, inside);
}
#endif

layout(location = 0) out vec4 outColor;

// Placeholder plains biome color (see BiomeColors.hpp) until per-block biome
// data is available from the server. Whether a face is tinted at all is
// still driven by the real per-texture whitelist, packed per-vertex.
const vec3 BIOME_COLOR_PLAINS = vec3(145.0 / 255.0, 189.0 / 255.0, 89.0 / 255.0);

// Light level -> brightness, bowed toward dark so dim light is common and
// only levels near 15 read as bright. falloff 3 is vanilla's
// level / (4 - 3 * level) (Lightmap.getBrightness); 0 is linear.
float LightCurve(float level, float falloff) {
    return level / (1.0 + falloff * (1.0 - level));
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

// Vanilla's cylindrical fog distance: looking up/down doesn't change it.
float FogFactor(vec3 offset) {
    float distance = max(length(offset.xz), abs(offset.y));
    return clamp((distance - camera.fogStart) / max(camera.fogEnd - camera.fogStart, 0.001), 0.0, 1.0);
}

// Blinn-Phong glint from the sun by day and a dimmer one from the moon at
// night, only on shiny blocks and only where the sky reaches. Returns the
// amount to add to the lit color.
vec3 Specular(vec3 viewOffset) {
    float shininess = fragSpecular.w;
    if (shininess <= 0.0 || camera.specularStrength <= 0.0 || camera.lightingEnabled < 0.5) return vec3(0.0);

    vec3 sun = camera.sunDirection.xyz;
    float day = smoothstep(-0.05, 0.15, sun.y);
    vec3 lightDir = normalize(mix(-sun, sun, day));
    float intensity = mix(0.2, 1.0, day);

    vec3 normal = fragSpecular.xyz;
    vec3 viewDir = normalize(-viewOffset);
    vec3 halfway = normalize(lightDir + viewDir);
    float facing = max(dot(normal, lightDir), 0.0);
    float highlight = pow(max(dot(normal, halfway), 0.0), mix(8.0, 128.0, shininess));

    float sky = fragSkyLight * fragSkyLight;
    return vec3(1.0) * highlight * facing * sky * intensity * mix(0.15, 1.0, shininess) * camera.specularStrength;
}

// BiomeColors.hpp's BiomeTint: 1 follows the biome (plains for now), 4-5 are vanilla's fixed spruce and birch colors.
vec3 BiomeColor(uint mode) {
    if (mode == 1u) return BIOME_COLOR_PLAINS;
    if (mode == 4u) return vec3(0x61, 0x99, 0x61) / 255.0;
    if (mode == 5u) return vec3(0x80, 0xA7, 0x55) / 255.0;
    return vec3(1.0);
}

void main() {
    vec4 texColor = texture(textureArray, vec3(fragUV, float(fragLayer)));
    vec3 tint = BiomeColor(fragBiomeTinted);

    // skyLightFactor scales the sky channel only — block light stays
    // constant day or night, matching a torch/glowstone.
    float skyLevel = fragSkyLight;
#ifdef SHADOWS
    // Shadow only dims the sky channel, so torches still light shaded ground.
    float sunFacing = dot(fragSpecular.xyz, camera.sunDirection.xyz);
    // A face turned away from the sun is in shadow by definition.
    float shadow = ShadowFactor(fragViewOffset, fragSpecular.xyz, max(sunFacing, 0.0)) * step(0.001, sunFacing);
    skyLevel *= mix(1.0 - camera.shadowParams.x, 1.0, shadow);
#endif
    float skyBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(skyLevel, camera.lightFalloff) * camera.skyLightFactor);
    float blockBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(fragBlockLight, camera.lightFalloff));
    float ambient = min(skyBrightness, AMBIENT_CEILING);
    float light = max(ambient, blockBrightness);

    // Apply AO to lighting.
    light *= fragAOMultiplier;

    // Final = (1-γ)*moody + γ*bright — vanilla's gamma blend. Fullbright
    // skips this entirely rather than blending toward γ=0.
    light = camera.lightingEnabled > 0.5 ? mix(light, NotGamma(light), camera.brightnessFactor) : 1.0;

    // fragFaceShade is fully resolved by terrain.vert (see its own comment
    // for why the branch lives there and not here) — a plain multiply, no
    // branching in this stage at all.
    vec3 color = texColor.rgb * tint * light * fragFaceShade + Specular(fragViewOffset);
#ifdef SHADOWS
    if (camera.shadowParams.w > 0.5) {
        // Debug view: blue outside the map, magenta where shadowed.
        bool outside = -dot(vec3(camera.view[0][2], camera.view[1][2], camera.view[2][2]), fragViewOffset) > camera.shadowSplits.z;
        color = outside ? mix(color, vec3(0.0, 0.2, 1.0), 0.6)
                        : mix(vec3(1.0, 0.0, 1.0), color, shadow);
    }
#endif
    vec3 foggedColor = mix(color, camera.fogColor.rgb, FogFactor(fragViewOffset));
    outColor = vec4(mix(camera.fogColor.rgb, foggedColor, fragChunkFade), texColor.a);
}
