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
} camera;

layout(location = 0) in vec3 inPosition; // EntityJsonModel::Vertex — world-local, feet at origin.
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// Per-instance record (vertex binding 1, instance rate). Entities, their
// parts and block entities with the same mesh and skin are one instanced
// draw; see EntityRenderer.cpp's TexturedBatch.
layout(location = 3) in vec4 inPositionYaw; // xyz: world position (feet), w: yaw radians.
layout(location = 4) in vec4 inHurtColor;   // rgb: hit-flash color, a: blend amount — same as entity.vert.
layout(location = 5) in vec4 inPartPivot;    // xyz: point the part turns about (model-local), w: tint as 0xRRGGBB (0 for none).
layout(location = 6) in vec4 inPartOffset;   // xyz: translation added after the turn (model-local), w: light (sky * 16 + block, each 0 to 15).
layout(location = 7) in vec4 inPartRotation; // xyz: radians, applied Z * Y * X. All zero for a still part.

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec4 fragHurt;
layout(location = 4) flat out vec2 fragLight;
layout(location = 5) flat out vec3 fragTint;
#ifdef SHADOWS
layout(location = 3) out vec3 fragViewOffset; // world position relative to the camera
#endif

void main() {
    float s = sin(inPositionYaw.w);
    float c = cos(inPositionYaw.w);
    mat3 yawRotation = mat3(
        c,   0.0, -s,
        0.0, 1.0, 0.0,
        s,   0.0, c
    );

    // Part pose: rotate about the pivot (Z * Y * X, as vanilla), then offset.
    float sx = sin(inPartRotation.x), cx = cos(inPartRotation.x);
    float sy = sin(inPartRotation.y), cy = cos(inPartRotation.y);
    float sz = sin(inPartRotation.z), cz = cos(inPartRotation.z);
    mat3 rotX = mat3(
        1.0, 0.0, 0.0,
        0.0, cx,  sx,
        0.0, -sx, cx
    );
    mat3 rotY = mat3(
        cy,  0.0, -sy,
        0.0, 1.0, 0.0,
        sy,  0.0, cy
    );
    mat3 rotZ = mat3(
        cz,  sz,  0.0,
        -sz, cz,  0.0,
        0.0, 0.0, 1.0
    );
    mat3 partRotation = rotZ * rotY * rotX;
    vec3 localPos = partRotation * (inPosition - inPartPivot.xyz) + inPartPivot.xyz + inPartOffset.xyz;
    vec3 localNormal = partRotation * inNormal;

    vec3 worldPos = yawRotation * localPos + inPositionYaw.xyz;
    gl_Position = camera.proj * camera.view * vec4(worldPos, 1.0);
#ifdef SHADOWS
    fragViewOffset = worldPos - camera.cameraPosition.xyz;
#endif

    fragNormal = yawRotation * localNormal;
    fragUV = inUV;
    fragHurt = inHurtColor;
    float sky = floor(inPartOffset.w / 16.0);
    float packedTint = inPartPivot.w;
    fragTint = packedTint > 0.0
        ? vec3(floor(packedTint / 65536.0), mod(floor(packedTint / 256.0), 256.0), mod(packedTint, 256.0)) / 255.0
        : vec3(1.0);
    fragLight = vec2(sky, inPartOffset.w - sky * 16.0) / 15.0;
}
