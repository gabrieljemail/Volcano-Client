#version 450

layout(set = 1, binding = 0) uniform sampler2D crackTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

// Blended as dst * src * 2, so mid-gray leaves the block alone and the crack pixels darken it.
void main() {
    vec4 crack = texture(crackTexture, fragUV);
    outColor = vec4(mix(vec3(0.5), crack.rgb, crack.a), 1.0);
}
