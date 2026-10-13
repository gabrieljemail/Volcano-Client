#version 450

layout(set = 0, binding = 0) uniform sampler2D skinTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in float fragLight;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texColor = texture(skinTexture, fragUV);
    if (texColor.a < 0.5) discard;
    outColor = vec4(texColor.rgb * fragLight, 1.0);
}
