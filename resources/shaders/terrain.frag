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

// Placeholder plains biome color (see BiomeColors.hpp) until per-block biome
// data is available from the server. Whether a face is tinted at all is
// still driven by the real per-texture whitelist, packed per-vertex.
const vec3 BIOME_COLOR_PLAINS = vec3(145.0 / 255.0, 189.0 / 255.0, 89.0 / 255.0);

// Vanilla's own per-level brightness curve (see Lightmap.getBrightness in
// the client sources — rawLevel/(4-3*rawLevel) for the overworld, where
// ambientLight is 0). A raw 0-15 light value does NOT correspond linearly to
// perceived brightness: this curve is steep near 0 and flattens out near 1,
// which is exactly why a flat `rawLevel/15` multiply (the old code) made a
// torch-lit corner (~11-13) and open sky (15) look almost identical —
// linearly that's 0.73-0.87 vs 1.0, barely different, while the real curve
// separates them much more (see BRIGHTNESS_FLOOR/CEILING below for how its
// [0,1] output gets remapped).
float LightCurve(float rawLevel01) {
    return rawLevel01 / (4.0 - 3.0 * rawLevel01);
}

// Remaps LightCurve's [0,1] output into the final brightness multiplier.
// CEILING is vanilla's own normal maximum (a raw 15 still renders at full
// brightness). FLOOR is half of vanilla's minimum-ambient-light constant —
// DimensionType.ambientLight() for the Nether is 0.1, the floor that keeps
// that dimension from ever reading pure black even at raw light 0; using
// half of it here (0.05) keeps the overworld's own unlit corners just
// barely above crushed black instead of exactly at it.
const float BRIGHTNESS_FLOOR = 0.05;
const float BRIGHTNESS_CEILING = 1.0;

// How brightly light reaching a surface ONLY through open sky is allowed to
// render, versus BRIGHTNESS_CEILING for an actual block-light source (a
// torch, lava, glowstone, ...) reaching the same surface. Capping the
// sky-only ceiling below the true maximum is what makes a light source read
// as glowing rather than just "as bright as a sunny day" — the same visual
// role real bloom/HDR would play, done here with a flat cap instead of an
// actual post-process pass.
const float AMBIENT_CEILING = 0.85;

void main() {
    vec4 texColor = texture(textureArray, vec3(fragUV, float(fragLayer)));
    vec3 tint = (fragBiomeTinted != 0u) ? BIOME_COLOR_PLAINS : vec3(1.0);

    float skyBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(fragSkyLight));
    float blockBrightness = mix(BRIGHTNESS_FLOOR, BRIGHTNESS_CEILING, LightCurve(fragBlockLight));
    float ambient = min(skyBrightness, AMBIENT_CEILING);
    float light = camera.lightingEnabled > 0.5 ? max(ambient, blockBrightness) : 1.0;

    outColor = vec4(texColor.rgb * tint * light, texColor.a);
}
