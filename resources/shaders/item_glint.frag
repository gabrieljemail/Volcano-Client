#version 450

layout(set = 0, binding = 0) uniform sampler2DArray textureArray;
layout(set = 1, binding = 0) uniform sampler2D glintTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in float fragLayer;
layout(location = 2) in float fragLight;
layout(location = 3) in float fragTime;

layout(location = 0) out vec4 outColor;

const float GLINT_STRENGTH = 0.75;
const float TILT = 0.17453293; // 10 degrees

// Vanilla's glint texture matrix: scale 8, tilt 10 degrees, scroll in x and y.
void main() {
    if (texture(textureArray, vec3(fragUV, fragLayer)).a < 0.1) discard;

    vec2 scaled = fragUV * 8.0;
    vec2 tilted = vec2(cos(TILT) * scaled.x - sin(TILT) * scaled.y, sin(TILT) * scaled.x + cos(TILT) * scaled.y);
    vec2 uv = tilted + vec2(-fract(fragTime / 27.5), fract(fragTime / 7.5));
    outColor = vec4(texture(glintTexture, uv).rgb * GLINT_STRENGTH, 1.0);
}
