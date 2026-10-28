#version 450

// Depth-only entity geometry for the shadow map: position only. The light's
// view-projection is a push constant for the whole batch; each instance
// brings its own model matrix (binding 1, instance rate), so a crowd of
// mobs is a few instanced draws (see EntityRenderer::RecordShadowCasters).
layout(push_constant) uniform Push {
    mat4 lightViewProj;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in mat4 inModel; // Occupies locations 1-4.

void main() {
    gl_Position = pc.lightViewProj * (inModel * vec4(inPosition, 1.0));
}
