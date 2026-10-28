#version 450

layout(set = 1, binding = 0) uniform sampler2D skinTexture;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec4 fragHurt;
layout(location = 5) flat in vec3 fragTint;
layout(location = 4) flat in vec2 fragLight; // sky, block: 0 to 1, sampled at the entity

layout(location = 0) out vec4 outColor;

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
#ifdef SHADOWS
    mat4 shadowViewProj[3];
    vec4 shadowParams; // x darkness, y texel (uv), w debug view
    vec4 shadowSplits; // far view distance of each cascade
    vec4 shadowTexels; // blocks per texel of each cascade
    vec4 shadowRanges; // depth span of each cascade, in blocks
#endif
} camera;

#ifdef SHADOWS
layout(location = 3) in vec3 fragViewOffset;

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
// Same light curve and gamma blend as terrain.frag.
float LightCurve(float level, float falloff) {
    return level / (1.0 + falloff * (1.0 - level));
}

float NotGamma(float level) {
    float inverted = 1.0 - level;
    return 1.0 - (inverted * inverted * inverted * inverted);
}

// Same fixed key light as entity.frag (the placeholder cube's shader) —
// the chunk light comes in per instance (fragLight).
const vec3 LIGHT_DIR = vec3(0.3838, 0.7845, 0.4885); // normalize(vec3(0.3, 1.0, 0.5))

void main() {
    vec4 texColor = texture(skinTexture, fragUV);
    if (texColor.a < 0.5) discard; // Skin sheets carry unused/transparent regions outside each box's own UV rect.

    float diffuse = max(dot(normalize(fragNormal), LIGHT_DIR), 0.0);
#ifdef SHADOWS
    // Shadow takes away the key light only; the 0.4 ambient stays.
    float sunFacing = max(dot(normalize(fragNormal), camera.sunDirection.xyz), 0.0);
    diffuse *= mix(1.0 - camera.shadowParams.x, 1.0, ShadowFactor(fragViewOffset, normalize(fragNormal), sunFacing));
#endif
    float brightness = 0.4 + 0.6 * diffuse;

    // The light at the entity's own position, through the same curve as terrain.
    float skyBrightness = mix(0.05, 1.0, LightCurve(fragLight.x, camera.lightFalloff) * camera.skyLightFactor);
    float blockBrightness = mix(0.05, 1.0, LightCurve(fragLight.y, camera.lightFalloff));
    float light = max(min(skyBrightness, 0.85), blockBrightness);
    light = camera.lightingEnabled > 0.5 ? mix(light, NotGamma(light), camera.brightnessFactor) : 1.0;
    vec3 shaded = texColor.rgb * fragTint * brightness * light;

    // Tick-invulnerability hit flash, same convention as entity.frag.
    vec3 finalColor = mix(shaded, fragHurt.rgb, fragHurt.a);
    outColor = vec4(finalColor, 1.0);
}
