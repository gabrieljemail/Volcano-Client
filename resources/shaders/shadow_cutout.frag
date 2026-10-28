#version 450

// Only opaque texels cast: leaves and plants keep their holes.
layout(set = 0, binding = 0) uniform sampler2DArray textureArray;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in uint fragLayer;

void main() {
    if (texture(textureArray, vec3(fragUV, float(fragLayer))).a < 0.5) discard;
}
