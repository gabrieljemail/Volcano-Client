#version 450

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in uint fragLayer;
layout(location = 2) in float fragLight;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texColor = texture(textureArray, vec3(fragUV, float(fragLayer)));
    outColor = vec4(texColor.rgb * fragLight, texColor.a);
}
