#version 450

layout(location = 0) out vec2 fragUV;

// One triangle covering the screen, from gl_VertexIndex alone: no vertex buffer.
void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    fragUV = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
