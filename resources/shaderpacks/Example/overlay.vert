#version 450

// Example shader pack overlay — see docs/RenderingEngineRework.md and
// ShaderPackPass.hpp for the interface. Emits one triangle that covers the
// whole screen from gl_VertexIndex alone (no vertex buffer is bound).

layout(push_constant) uniform Overlay {
    vec4 viewport; // x: width px, y: height px, z: time s, w: delta time s
    vec4 sky;      // rgb: sky color, a: day brightness (0 midnight .. 1 noon)
    vec4 camera;   // xyz: eye world position, w: 1 lighting on / 0 off
} overlay;

layout(location = 0) out vec2 fragUV;

void main() {
    fragUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(fragUV * 2.0 - 1.0, 0.0, 1.0);
}
