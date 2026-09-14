#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    float lightingEnabled;
} camera;

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in uint fragLayer;
layout(location = 2) in float fragLight;
layout(location = 3) flat in uint fragBiomeTinted;

layout(location = 0) out vec4 outColor;

// Placeholder plains biome color (see BiomeColors.hpp) until per-block biome
// data is available from the server. Whether a face is tinted at all is
// still driven by the real per-texture whitelist, packed per-vertex.
const vec3 BIOME_COLOR_PLAINS = vec3(145.0 / 255.0, 189.0 / 255.0, 89.0 / 255.0);

void main() {
    vec4 texColor = texture(textureArray, vec3(fragUV, float(fragLayer)));
    vec3 tint = (fragBiomeTinted != 0u) ? BIOME_COLOR_PLAINS : vec3(1.0);
    float light = camera.lightingEnabled > 0.5 ? fragLight : 1.0;
    outColor = vec4(texColor.rgb * tint * light, texColor.a);
}
