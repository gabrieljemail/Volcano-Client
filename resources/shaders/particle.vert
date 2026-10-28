#version 450

// One instance per particle: a camera-facing quad, drawn as six vertices.
layout(location = 0) in vec4 inPositionSize; // Camera-relative position, then the edge length.
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec4 inUv;           // u0, v0, u1, v1
layout(location = 3) in vec2 inLayerRoll;

layout(push_constant) uniform Push {
    mat4 viewProj;                           // Projection * the view's rotation: positions are camera-relative.
    vec4 right;
    vec4 up;
} push;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;
layout(location = 2) flat out float outLayer;

const vec2 CORNERS[6] = vec2[](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main()
{
    vec2 corner = CORNERS[gl_VertexIndex];
    float s = sin(inLayerRoll.y);
    float c = cos(inLayerRoll.y);
    vec2 rolled = vec2(c * corner.x - s * corner.y, s * corner.x + c * corner.y);

    vec3 world = inPositionSize.xyz + (push.right.xyz * rolled.x + push.up.xyz * rolled.y) * (inPositionSize.w * 0.5);
    gl_Position = push.viewProj * vec4(world, 1.0);

    vec2 t = corner * 0.5 + 0.5;
    outUv = vec2(mix(inUv.x, inUv.z, t.x), mix(inUv.w, inUv.y, t.y)); // Up is the top row of the texture.
    outColor = inColor;
    outLayer = inLayerRoll.x;
}
