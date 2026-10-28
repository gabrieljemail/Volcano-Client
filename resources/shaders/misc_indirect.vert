#version 450

// Indirect-draw twin of misc.vert: the per-section model translation and fade
// come from a per-instance vertex attribute (written by ChunkDrawBatch, one
// instance per section) instead of push constants, so a whole list of
// sections draws with one vkCmdDrawIndexedIndirect. Keep the rest identical
// to misc.vert.


// Full layout through cameraPosition (std140 offsets are sequential).
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

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in uint inPacked;
layout(location = 3) in vec4 sectionData; // xyz = section world origin, w = fade

layout(location = 0) out vec2 fragUV;
layout(location = 1) flat out uint fragLayer;
layout(location = 2) out float fragSkyLight;
layout(location = 3) flat out uint fragBiomeTint;
layout(location = 4) out float fragBlockLight;
layout(location = 5) out vec3 fragViewOffset; // world position relative to the camera, for fog
layout(location = 6) out float fragChunkFade;
layout(location = 7) out float fragFoliageHeight;

void main() {
    // See MiscVertex::Pack: textureLayer (0-15), blockLight (16-19), skyLight
    // (20-23), tintMode (24-25: 0=none, 1=foliage, 2=water).
    fragLayer = bitfieldExtract(inPacked, 0, 16);
    uint blockLight = bitfieldExtract(inPacked, 16, 4);
    uint skyLight = bitfieldExtract(inPacked, 20, 4);
    fragBiomeTint = bitfieldExtract(inPacked, 24, 2);

    vec4 worldPos = vec4(inPosition + sectionData.xyz, 1.0);
    gl_Position = camera.proj * camera.view * worldPos;
    fragViewOffset = worldPos.xyz - camera.cameraPosition.xyz;

    fragUV = inUV;
    // Kept apart — see terrain.frag's own comment (misc.frag applies the
    // exact same curve/bloom-cap logic).
    fragSkyLight = float(skyLight) / 15.0;
    fragBlockLight = float(blockLight) / 15.0;
    fragChunkFade = sectionData.w;
    fragFoliageHeight = clamp(1.0 - inUV.y, 0.0, 1.0);
}
