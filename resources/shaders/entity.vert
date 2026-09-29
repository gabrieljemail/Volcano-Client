#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} camera;

struct EntityInstance {
    vec4 positionYaw;     // xyz: world position (feet), w: yaw radians.
    vec4 halfExtents;     // xyz: half bounding-box extents, w: unused.
    vec4 color;           // rgb: base tint, a: unused.
    vec4 hurtColor;       // rgb: hit-flash color, a: blend amount (0 when not hurt).
};

// GPU-driven batching: the CPU only uploads per-entity data here and issues
// one indirect draw — this shader fetches each instance's own transform via
// gl_InstanceIndex instead of the CPU binding per-entity push constants.
layout(std430, set = 1, binding = 0) readonly buffer EntityInstances {
    EntityInstance instances[];
};

// Local position within a -0.5..0.5 unit box — the placeholder cube, or one
// of QuadrupedModel/GolemModel's multi-box meshes (see EntityRenderer.cpp's
// EntityShapeSlot). Same scaling applies to all of them: X/Z as fractions of
// halfExtents.x (footprint), Y as fractions of halfExtents.y (height).
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec4 fragHurt;

void main() {
    EntityInstance inst = instances[gl_InstanceIndex];

    vec3 halfExtents = inst.halfExtents.xyz;
    float yaw = inst.positionYaw.w;

    float s = sin(yaw);
    float c = cos(yaw);
    mat3 yawRotation = mat3(
        c,   0.0, -s,
        0.0, 1.0, 0.0,
        s,   0.0, c
    );

    vec3 localPos = inPosition * (halfExtents * 2.0);
    vec3 rotatedPos = yawRotation * localPos;
    // Entity position is feet/base (Minecraft convention) — lift the cube up
    // by its own half-height so it spans position.y .. position.y + height.
    vec3 worldPos = rotatedPos + inst.positionYaw.xyz + vec3(0.0, halfExtents.y, 0.0);

    gl_Position = camera.proj * camera.view * vec4(worldPos, 1.0);

    fragNormal = yawRotation * inNormal;
    fragColor = inst.color;
    fragHurt = inst.hurtColor;
}
