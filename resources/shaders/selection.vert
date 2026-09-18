#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} camera;

// One box (a block's collision shape can have more than one — a stair,
// say — so SelectionRenderer issues one draw per box) plus the color to
// tint it, either the outline or the fill depending on which pipeline this
// draw is bound to. worldMin/worldMax are already in world space (block
// origin + the shape's own local box, see SelectionRenderer.cpp) — the
// vertex shader only needs to interpolate across them.
layout(push_constant) uniform PushConstants {
    vec4 worldMin;
    vec4 worldMax;
    vec4 color;
} pc;

layout(location = 0) in vec3 inPosition; // Unit cube corner, 0..1 per axis.

layout(location = 0) out vec4 fragColor;

void main() {
    vec3 worldPos = mix(pc.worldMin.xyz, pc.worldMax.xyz, inPosition);
    gl_Position = camera.proj * camera.view * vec4(worldPos, 1.0);
    fragColor = pc.color;
}
