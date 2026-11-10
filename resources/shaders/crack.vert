#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} camera;

// One box of the broken block's shape, in world space.
layout(push_constant) uniform PushConstants {
    vec4 worldMin;
    vec4 worldMax;
} pc;

layout(location = 0) in vec3 inPosition; // Unit cube corner, 0..1 per axis.
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;

void main() {
    vec3 worldPos = mix(pc.worldMin.xyz, pc.worldMax.xyz, inPosition);
    gl_Position = camera.proj * camera.view * vec4(worldPos, 1.0);
    fragUV = inUV;
}
