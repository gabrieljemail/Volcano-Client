#version 450

// Depth-only terrain for the shadow map: unpacks a chunk vertex's position
// (see terrain.vert) and projects it with the light's matrix times the
// section's model matrix, premultiplied on the CPU.
layout(push_constant) uniform Push {
    mat4 lightMvp;
} pc;

layout(location = 0) in uvec3 packedData;

void main() {
    float x = float(bitfieldExtract(packedData.x, 0, 6));
    float z = float(bitfieldExtract(packedData.x, 6, 6));
    float y = float(bitfieldExtract(packedData.x, 12, 9));
    gl_Position = pc.lightMvp * vec4(x, y, z, 1.0);
}
