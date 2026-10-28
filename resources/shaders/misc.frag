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
layout(location = 3) flat in uint fragBiomeTint;
layout(location = 4) in float fragBlockLight;
layout(location = 5) in vec3 fragViewOffset;
layout(location = 6) in float fragChunkFade;
layout(location = 7) in float fragFoliageHeight;
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

// Same placeholder biome tint as terrain.frag — see its own comment — plus a
// second placeholder for water (BiomeColors.hpp's BiomeTint::Water): water's
// own texture is grayscale and relies entirely on this multiply to read as
// blue, same reason grass/foliage need BIOME_COLOR_PLAINS.
const vec3 BIOME_COLOR_PLAINS = vec3(145.0 / 255.0, 189.0 / 255.0, 89.0 / 255.0);
const vec3 BIOME_COLOR_WATER = vec3(0x3F / 255.0, 0x76 / 255.0, 0xE4 / 255.0);

// Same curve/constants/fog as terrain.frag — duplicated, no GLSL include mechanism.
float LightCurve(float level, float falloff) {
    return level / (1.0 + falloff * (1.0 - level));
}

float FogFactor(vec3 offset) {
    float distance = max(length(offset.xz), abs(offset.y));
    return clamp((distance - camera.fogStart) / max(camera.fogEnd - camera.fogStart, 0.001), 0.0, 1.0);
}

const float BRIGHTNESS_FLOOR = 0.05;
const float BRIGHTNESS_CEILING = 1.0;
const float AMBIENT_CEILING = 0.85;

// Same as terrain.frag's NotGamma.
float NotGamma(float level) {
    float inverted = 1.0 - level;
    return 1.0 - (inverted * inverted * inverted * inverted);
}

// Water: a rippled normal from screen-space derivatives, a sun/moon glint,
// and a Fresnel blend toward the horizon color standing in for a sky
// reflection. Returns the color to add and raises `alpha` at grazing angles.
vec3 WaterSurface(vec3 viewOffset, inout float alpha, out float reflection) {
    reflection = 0.0;
    if (camera.specularStrength <= 0.0 || camera.lightingEnabled < 0.5) return vec3(0.0);

    // Face normal from the position derivatives, turned toward the camera.
    vec3 normal = normalize(cross(dFdx(viewOffset), dFdy(viewOffset)));
    vec3 viewDir = normalize(-viewOffset);
    if (dot(normal, viewDir) < 0.0) normal = -normal;

    // Ripples on up-facing water only, drifting with time.
    vec3 world = viewOffset + camera.cameraPosition.xyz;
    float flatness = smoothstep(0.7, 0.95, normal.y);
    vec2 ripple = vec2(sin(world.x * 2.3 + camera.time * 1.4) + sin(world.z * 1.7 - camera.time * 1.1),
                       sin(world.z * 2.1 + camera.time * 1.3) + sin(world.x * 1.9 + camera.time * 0.9));
    normal = normalize(normal + vec3(ripple.x, 0.0, ripple.y) * 0.025 * flatness);

    vec3 sun = camera.sunDirection.xyz;
    float day = smoothstep(-0.05, 0.15, sun.y);
    vec3 lightDir = normalize(mix(-sun, sun, day));
    float intensity = mix(0.25, 1.0, day);

    float sky = fragSkyLight * fragSkyLight;
    vec3 halfway = normalize(lightDir + viewDir);
    float glint = pow(max(dot(normal, halfway), 0.0), 160.0) * max(dot(normal, lightDir), 0.0);

    float fresnel = pow(1.0 - max(dot(normal, viewDir), 0.0), 4.0);
    reflection = fresnel * sky * camera.specularStrength;
    alpha = mix(alpha, 1.0, fresnel * 0.5 * camera.specularStrength);
    return vec3(1.0) * glint * sky * intensity * 1.5 * camera.specularStrength;
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

    vec3 tint = vec3(1.0);
    if (fragBiomeTint == 1u || fragBiomeTint == 3u) tint = BIOME_COLOR_PLAINS;
    else if (fragBiomeTint == 2u) tint = BIOME_COLOR_WATER;
    else if (fragBiomeTint == 4u) tint = vec3(0x61, 0x99, 0x61) / 255.0; // Spruce.
    else if (fragBiomeTint == 5u) tint = vec3(0x80, 0xA7, 0x55) / 255.0; // Birch.
    else if (fragBiomeTint == 6u) tint = vec3(0x20, 0x80, 0x30) / 255.0; // Lily pad.

    // Same skyLightFactor/gamma treatment as terrain.frag.
    float skyLevel = fragSkyLight;
#ifdef SHADOWS
    // Shadow only dims the sky channel, so torches still light shaded ground.
    // No face normal here, so no offset and the largest bias (edge-on).
    skyLevel *= mix(1.0 - camera.shadowParams.x, 1.0, ShadowFactor(fragViewOffset, vec3(0.0), 0.0));
#endif
    float skyBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(skyLevel, camera.lightFalloff) * camera.skyLightFactor);
    float blockBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(fragBlockLight, camera.lightFalloff));
    float ambient = min(skyBrightness, AMBIENT_CEILING);
    float light = max(ambient, blockBrightness);
    light = camera.lightingEnabled > 0.5 ? mix(light, NotGamma(light), camera.brightnessFactor) : 1.0;

    float foliageShade = fragBiomeTint == 3u ? mix(0.55, 1.0, smoothstep(0.0, 1.0, fragFoliageHeight)) : 1.0;
    vec3 color = texColor.rgb * tint * light * foliageShade;
    float alpha = texColor.a;
    if (fragBiomeTint == 2u) {
        float reflection;
        vec3 glint = WaterSurface(fragViewOffset, alpha, reflection);
        color = mix(color, camera.fogColor.rgb, reflection * 0.6) + glint;
    }
    vec3 foggedColor = mix(color, camera.fogColor.rgb, FogFactor(fragViewOffset));
    outColor = vec4(mix(camera.fogColor.rgb, foggedColor, fragChunkFade), alpha);
}
