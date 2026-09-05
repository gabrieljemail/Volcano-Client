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
layout(location = 2) out float fragLight;
layout(location = 3) flat out uint fragBiomeTinted;

void main() {
    uint word0 = packedData.x;
    uint word1 = packedData.y;
    uint word2 = packedData.z;

    // Extract position (6 bits each)
    float x = float(bitfieldExtract(word0, 0, 6));
    float y = float(bitfieldExtract(word0, 6, 6));
    float z = float(bitfieldExtract(word0, 12, 6));

    // Extract biome-tint flag (see BiomeColors.hpp for the placeholder color).
    fragBiomeTinted = bitfieldExtract(word0, 23, 1);

    // Extract lighting
    uint blockLight = bitfieldExtract(word1, 16, 4);
    uint skyLight   = bitfieldExtract(word1, 20, 4);
    
    // Extract texture layer (16 bits)
    uint textureLayer = bitfieldExtract(word1, 0, 16);
    
    // Extract UV coordinates (8 bits each, normalized to 0-1)
    float u = float(bitfieldExtract(word2, 0, 8)) / 255.0;
    float v = float(bitfieldExtract(word2, 8, 8)) / 255.0;

    vec3 localPos = vec3(x, y, z);
    vec4 worldPos = pc.model * vec4(localPos, 1.0);
    gl_Position = camera.proj * camera.view * worldPos;

    fragUV = vec2(u, v);
    fragLayer = textureLayer;
    fragLight = max(float(blockLight), float(skyLight)) / 15.0;
}
