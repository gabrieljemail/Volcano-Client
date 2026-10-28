#version 450

// Depth-only cutout geometry (leaves, plants, glass frames, slabs...) for the
// shadow map: MiscVertex in, the light's matrix times the section's model
// matrix premultiplied on the CPU. The texture layer rides along so the
// fragment stage can alpha-test.
layout(push_constant) uniform Push {
    mat4 lightMvp;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in uint inPacked;

layout(location = 0) out vec2 fragUV;
layout(location = 1) flat out uint fragLayer;

void main() {
    gl_Position = pc.lightMvp * vec4(inPosition, 1.0);
    fragUV = inUV;
    fragLayer = bitfieldExtract(inPacked, 0, 16);
}
