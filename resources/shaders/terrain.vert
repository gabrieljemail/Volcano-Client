#version 450

layout(push_constant) uniform PushConstants {
    mat4 model;
    float fade;
} pc;

// Full CameraUBO layout, even though this stage only reads the shade
// fields below — std140 offsets are sequential by declaration order, so
// every preceding field has to be declared to land shadeTop etc. at the
// right offset. Must stay byte-identical to CameraUBO.hpp and terrain.frag.
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
} camera;

layout(location = 0) in uvec3 packedData;

layout(location = 0) out vec2 fragUV;
layout(location = 1) flat out uint fragLayer;
layout(location = 2) out float fragSkyLight;
layout(location = 3) flat out uint fragBiomeTinted;
layout(location = 4) out float fragBlockLight;
layout(location = 5) out float fragAOMultiplier;
layout(location = 6) out float fragFaceShade;
layout(location = 7) out vec3 fragViewOffset; // world position relative to the camera, for fog
layout(location = 8) out float fragChunkFade;

// Vanilla's hardcoded per-face diffuse shading (LevelRenderer::getShade).
// normalIndex matches ChunkMesher's FaceIndexFor: 0=Up, 1=Down, 2=North,
// 3=South, 4=East, 5=West. Computed here (per-vertex, a handful of
// invocations) rather than in terrain.frag (per-pixel, potentially
// millions) — branching on this in the fragment shader reliably hung this
// GPU's driver hard enough to trip Windows' TDR watchdog; the exact same
// branches here, evaluated far less often, don't.
float FaceShade(uint normalIndex) {
    if (normalIndex == 0u) return camera.shadeTop;
    if (normalIndex == 1u) return camera.shadeBottom;
    if (normalIndex == 2u || normalIndex == 3u) return camera.shadeNorthSouth;
    return camera.shadeEastWest; // 4 (East) or 5 (West).
}

void main() {
    uint word0 = packedData.x;
    uint word1 = packedData.y;
    uint word2 = packedData.z;

    // Extract position: x/z (6 bits each, 0-15), y (9 bits, 0-383 — chunks
    // span the full -64..319 world-height range, offset via the model matrix)
    float x = float(bitfieldExtract(word0, 0, 6));
    float z = float(bitfieldExtract(word0, 6, 6));
    float y = float(bitfieldExtract(word0, 12, 9));

    // Extract biome-tint flag (see BiomeColors.hpp for the placeholder color).
    fragBiomeTinted = bitfieldExtract(word0, 26, 1);

    // Extract face normal index (see FaceShade's own comment) and resolve
    // it to a shade multiplier right here rather than passing the raw index
    // through to terrain.frag.
    uint normalIndex = bitfieldExtract(word0, 21, 3);
    fragFaceShade = FaceShade(normalIndex);

    // Extract lighting
    uint blockLight = bitfieldExtract(word1, 16, 4);
    uint skyLight   = bitfieldExtract(word1, 20, 4);

    // Extract texture layer (16 bits)
    uint textureLayer = bitfieldExtract(word1, 0, 16);

    // Extract AO
    uint ao = bitfieldExtract(word0, 24, 2);
    fragAOMultiplier = mix(1.0, 0.2, float(ao) / 3.0);

    // Extract UV coordinates (8 bits each). These are raw block-space tile
    // counts, not normalized 0-1 — a single-block face still spans exactly
    // one tile (0 or 1), while a greedy-meshed quad spanning multiple blocks
    // uses larger values so the texture sampler (REPEAT wrap mode) tiles the
    // texture once per block instead of stretching it across the whole quad.
    float u = float(bitfieldExtract(word2, 0, 8));
    float v = float(bitfieldExtract(word2, 8, 8));

    vec3 localPos = vec3(x, y, z);
    vec4 worldPos = pc.model * vec4(localPos, 1.0);
    gl_Position = camera.proj * camera.view * worldPos;
    fragViewOffset = worldPos.xyz - camera.cameraPosition.xyz;

    fragUV = vec2(u, v);
    fragLayer = textureLayer;
    // Kept apart rather than combined here — terrain.frag's own curve caps
    // sky-only light lower than block light, to make torches/lava read as
    // actual light sources instead of the same flat brightness as daylight.
    fragSkyLight = float(skyLight) / 15.0;
    fragBlockLight = float(blockLight) / 15.0;
    fragChunkFade = pc.fade;
}
