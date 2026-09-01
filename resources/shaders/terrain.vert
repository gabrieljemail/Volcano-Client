#version 450

layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} camera;

layout(location = 0) in uvec2 packedData;

layout(location = 0) out vec2 fragUV;
layout(location = 1) flat out uint fragLayer;
layout(location = 2) out float fragLight;

// Placeholder palette until a real texture atlas exists.
// Index order must match BlockType enum values.
vec3 PaletteColor(uint index) {
    vec3 palette[6] = vec3[](
        vec3(1.0, 1.0, 1.0), // 0: default/unknown
        vec3(0.5, 0.5, 0.5), // Stone
        vec3(0.6, 0.4, 0.2), // Dirt
        vec3(0.3, 0.8, 0.2), // Grass
        vec3(0.4, 0.2, 0.0), // Wood
        vec3(0.1, 0.5, 0.1)  // Leaves
    );
    return palette[min(index, 5u)];
}

void main() {
    uint word0 = packedData.x;
    uint word1 = packedData.y;

    float x = float(bitfieldExtract(word0, 0, 6));
    float y = float(bitfieldExtract(word0, 6, 6));
    float z = float(bitfieldExtract(word0, 12, 6));
    uint ao = bitfieldExtract(word0, 21, 2);

    uint palette    = bitfieldExtract(word1, 2, 14);
    uint blockLight = bitfieldExtract(word1, 16, 4);
    uint skyLight   = bitfieldExtract(word1, 20, 4);

    vec3 localPos = vec3(x, y, z);
    vec4 worldPos = pc.model * vec4(localPos, 1.0);
    gl_Position = camera.proj * camera.view * worldPos;

    fragColor = PaletteColor(palette);
    fragLight = max(float(blockLight), float(skyLight)) / 15.0;
}
