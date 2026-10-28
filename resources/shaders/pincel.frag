#version 450

layout(set = 0, binding = 0) uniform sampler2D pincelTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

// Pincel's pixels are premultiplied, so the pipeline blends with ONE / ONE_MINUS_SRC_ALPHA.
void main() {
    outColor = texture(pincelTexture, fragUV);
}
