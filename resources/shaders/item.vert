#version 450

// Draws one item mesh (see ItemModel) with a texture-array layer per vertex.
// Everything per-draw is in push constants: the display transform has
// already been folded into mvp on the CPU.
layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 normalRow0; // Rows of the 3x3 normal matrix (rotation part of the display transform).
    vec4 normalRow1;
    vec4 normalRow2;
    vec4 params;     // x: 1 = flat item lit head-on, 0 = block-style lighting.
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in float inLayer;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out float fragLayer;
layout(location = 2) out float fragLight;

// Vanilla's item lights, in view space.
const vec3 LIGHT0 = vec3(0.1569, 0.7845, -0.5492); // normalize(vec3(0.2, 1.0, -0.7))
const vec3 LIGHT1 = vec3(-0.1569, 0.7845, 0.5492); // normalize(vec3(-0.2, 1.0, 0.7))

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);

    vec3 normal = normalize(vec3(
        dot(pc.normalRow0.xyz, inNormal),
        dot(pc.normalRow1.xyz, inNormal),
        dot(pc.normalRow2.xyz, inNormal)));
    float lit = max(dot(normal, LIGHT0), 0.0) + max(dot(normal, LIGHT1), 0.0);

    fragUV = inUV;
    fragLayer = inLayer;
    fragLight = pc.params.x > 0.5 ? 1.0 : min(1.0, 0.4 + 0.6 * lit);
}
