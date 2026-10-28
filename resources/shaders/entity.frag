#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec4 fragHurt;

layout(location = 0) out vec4 outColor;

#ifdef SHADOWS
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

// No lighting system reaches entities yet (they don't carry chunk sky/block
// light values), so a fixed key light gives the placeholder box some shape
// instead of flat-shading it into a silhouette.
const vec3 LIGHT_DIR = vec3(0.3838, 0.7845, 0.4885); // normalize(vec3(0.3, 1.0, 0.5))

void main() {
    float diffuse = max(dot(normalize(fragNormal), LIGHT_DIR), 0.0);
#ifdef SHADOWS
    // Shadow takes away the key light only; the 0.4 ambient stays.
    float sunFacing = max(dot(normalize(fragNormal), camera.sunDirection.xyz), 0.0);
    diffuse *= mix(1.0 - camera.shadowParams.x, 1.0, ShadowFactor(fragViewOffset, normalize(fragNormal), sunFacing));
#endif
    float brightness = 0.4 + 0.6 * diffuse;
    vec3 shaded = fragColor.rgb * brightness;

    // Hit flash: Graphics.EntityHurt.Color, blended in by its alpha.
    vec3 finalColor = mix(shaded, fragHurt.rgb, fragHurt.a);
    outColor = vec4(finalColor, fragColor.a);
}
