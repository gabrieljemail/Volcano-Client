#version 450

// The first-person arm: one skinned box, placed entirely by mvp.
layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 shade; // x: brightness from the light around the player.
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out float fragLight;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragUV = inUV;
    // Vanilla's per-face shading; the normal is in the arm's own space.
    vec3 n = abs(inNormal);
    fragLight = n.y > 0.5 ? (inNormal.y < 0.0 ? 1.0 : 0.5) : (n.z > 0.5 ? 0.8 : 0.6);
    fragLight *= pc.shade.x;
}
