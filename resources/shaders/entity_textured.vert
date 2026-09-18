#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} camera;

// Per-draw, not per-instance (unlike the placeholder cube's EntityInstances
// SSBO) — humanoid entities (player/zombie/mannequin) are drawn one at a
// time, one bound skin texture per draw, rather than batched. See
// EntityRenderer.cpp's own comment on why.
layout(push_constant) uniform PushConstants {
    vec4 positionYaw; // xyz: world position (feet), w: yaw radians.
    float hurtAmount; // 0-1 red hit-flash intensity, same convention as the placeholder cube.
} pc;

layout(location = 0) in vec3 inPosition; // HumanoidModel::Vertex — world-local, feet at origin.
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out float fragHurt;

void main() {
    float s = sin(pc.positionYaw.w);
    float c = cos(pc.positionYaw.w);
    mat3 yawRotation = mat3(
        c,   0.0, -s,
        0.0, 1.0, 0.0,
        s,   0.0, c
    );

    vec3 worldPos = yawRotation * inPosition + pc.positionYaw.xyz;
    gl_Position = camera.proj * camera.view * vec4(worldPos, 1.0);

    fragNormal = yawRotation * inNormal;
    fragUV = inUV;
    fragHurt = pc.hurtAmount;
}
