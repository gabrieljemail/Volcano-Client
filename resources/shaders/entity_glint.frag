#version 450

layout(set = 1, binding = 0) uniform sampler2D armorTexture;
layout(set = 2, binding = 0) uniform sampler2D glintTexture;

layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    float lightingEnabled;
    float brightnessFactor;
    float skyLightFactor;
    float lightFalloff;
    float shadeTop;
    float shadeBottom;
    float shadeNorthSouth;
    float shadeEastWest;
    vec4 fogColor;
    vec4 cameraPosition;
    float fogStart;
    float fogEnd;
    float specularStrength;
    float time;
    vec4 sunDirection;
} camera;

const float GLINT_STRENGTH = 0.75;
const float TILT = 0.17453293; // 10 degrees

// Vanilla's entity glint matrix: scale 0.16, tilt 10 degrees, scroll in x and y.
void main() {
    if (texture(armorTexture, fragUV).a < 0.5) discard;

    vec2 scaled = fragUV * 0.16;
    vec2 tilted = vec2(cos(TILT) * scaled.x - sin(TILT) * scaled.y, sin(TILT) * scaled.x + cos(TILT) * scaled.y);
    vec2 uv = tilted + vec2(-fract(camera.time / 27.5), fract(camera.time / 7.5));
    outColor = vec4(texture(glintTexture, uv).rgb * GLINT_STRENGTH, 1.0);
}
