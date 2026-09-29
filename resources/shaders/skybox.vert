#version 450

// Unit cube from gl_VertexIndex (36 vertices, no vertex buffer) — the same
// cube vanilla's CubeMap draws. Built arithmetically rather than from a
// constant array to stay on the simple path the Intel driver handles well.
layout(push_constant) uniform Push {
    mat4 mvp;
} push;

layout(location = 0) out vec3 texCoord;

void main()
{
    int face = gl_VertexIndex / 6;
    int k = gl_VertexIndex % 6;

    // Two triangles per face: corners 0,1,2 then 0,2,3.
    int corner = (k < 3) ? k : ((k == 3) ? 0 : (k - 2));
    vec2 uv = vec2((corner == 1 || corner == 2) ? 1.0 : -1.0,
                   (corner >= 2) ? 1.0 : -1.0);

    float s = ((face & 1) == 0) ? 1.0 : -1.0;
    int axis = face >> 1;
    vec3 pos = (axis == 0) ? vec3(s, uv.x, uv.y)
             : ((axis == 1) ? vec3(uv.x, s, uv.y) : vec3(uv.x, uv.y, s));

    texCoord = pos;

    // z = w pins the cube to the far plane regardless of the projection's
    // depth range, so it can never be clipped by it.
    gl_Position = (push.mvp * vec4(pos, 1.0)).xyww;
}
