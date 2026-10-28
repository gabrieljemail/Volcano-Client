#version 450

// Cloud geometry is built camera-relative on the CPU (see CloudPass), so the
// push matrix carries the rotation-only view and the cloud grid's offset from
// the camera - that keeps float precision at any world coordinate.
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;   // rgb, a = overall opacity
    vec4 offset;  // xyz: grid offset from the camera, w = fade end distance
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in float inShade;

layout(location = 0) out float fragShade;
layout(location = 1) out float fragDistance;

void main() {
    gl_Position = push.mvp * vec4(inPosition, 1.0);
    fragShade = inShade;
    fragDistance = length((inPosition + push.offset.xyz).xz);
}
