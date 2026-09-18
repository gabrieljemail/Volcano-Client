#version 450

layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} camera;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in uint inPacked;

layout(location = 0) out vec2 fragUV;
layout(location = 1) flat out uint fragLayer;
layout(location = 2) out float fragSkyLight;
layout(location = 3) flat out uint fragBiomeTinted;
layout(location = 4) out float fragBlockLight;

void main() {
    // See MiscVertex::Pack: textureLayer (0-15), blockLight (16-19), skyLight (20-23), biomeTinted (bit 24).
    fragLayer = bitfieldExtract(inPacked, 0, 16);
    uint blockLight = bitfieldExtract(inPacked, 16, 4);
    uint skyLight = bitfieldExtract(inPacked, 20, 4);
    fragBiomeTinted = bitfieldExtract(inPacked, 24, 1);

    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    gl_Position = camera.proj * camera.view * worldPos;

    fragUV = inUV;
    // Kept apart — see terrain.frag's own comment (misc.frag applies the
    // exact same curve/bloom-cap logic).
    fragSkyLight = float(skyLight) / 15.0;
    fragBlockLight = float(blockLight) / 15.0;
}
