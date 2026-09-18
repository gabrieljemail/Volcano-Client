#version 450

layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} camera;

layout(location = 0) in uvec3 packedData;

layout(location = 0) out vec2 fragUV;
layout(location = 1) flat out uint fragLayer;
layout(location = 2) out float fragSkyLight;
layout(location = 3) flat out uint fragBiomeTinted;
layout(location = 4) out float fragBlockLight;

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

    // Extract lighting
    uint blockLight = bitfieldExtract(word1, 16, 4);
    uint skyLight   = bitfieldExtract(word1, 20, 4);
    
    // Extract texture layer (16 bits)
    uint textureLayer = bitfieldExtract(word1, 0, 16);
    
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

    fragUV = vec2(u, v);
    fragLayer = textureLayer;
    // Kept apart rather than combined here — terrain.frag's own curve caps
    // sky-only light lower than block light, to make torches/lava read as
    // actual light sources instead of the same flat brightness as daylight.
    fragSkyLight = float(skyLight) / 15.0;
    fragBlockLight = float(blockLight) / 15.0;
}
