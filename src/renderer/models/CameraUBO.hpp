#pragma once
#ifndef CAMERA_UBO_H
#define CAMERA_UBO_H

#include <cstdint>
#include <glm/glm.hpp>

namespace Volcano {

struct CameraUBO {
    glm::mat4 view;
    glm::mat4 proj;
    // 1.0 = normal per-block lighting, 0.0 = full-bright ("G" toggle). Float
    // for std140, not bool.
    float lightingEnabled = 1.0f;

    // Vanilla's Brightness slider (0=Moody, 1=Bright) - see terrain.frag's NotGamma.
    float brightnessFactor = 0.5f;

    // Sky-light channel's day/night multiplier (0 at midnight, 1 at noon) -
    // block light stays constant, matching vanilla.
    float skyLightFactor = 1.0f;

    // LightCurve's falloff (Graphics.LightFalloff): 0 = linear, 3 = vanilla.
    float lightFalloff = 3.0f;

    // Vanilla's hardcoded per-face diffuse shading (LevelRenderer::getShade)
    // - a flat multiplier by which cardinal direction a face points, giving
    // terrain a "lit from above" look independent of the real light level.
    // Configurable via Graphics.Shading.* rather than hardcoded in the
    // shader so a shader pack (or a future "flat lighting" toggle) can
    // override it. Indexed in terrain.frag by the packed vertex's
    // normalIndex - see ChunkMesher's FaceIndexFor for the Up/Down/North/
    // South/East/West -> 0-5 mapping this must match (North and South share
    // shadeNorthSouth, East and West share shadeEastWest, same as vanilla).
    float shadeTop = 1.0f;
    float shadeBottom = 0.4f;
    float shadeNorthSouth = 0.8f;
    float shadeEastWest = 0.6f;

    // Distance fog toward the horizon sky color (rgb; w unused), measured
    // cylindrically from cameraPosition (xyz; w unused). std140 offsets 160,
    // 176, 192, 196 - must match every shader that declares them.
    glm::vec4 fogColor{0.0f};
    glm::vec4 cameraPosition{0.0f};
    float fogStart = 0.0f;
    float fogEnd = 0.0f;

    // Specular on terrain and water: overall strength (0 = off,
    // Graphics.Specular), seconds since start (water ripples), then the
    // direction toward the sun (xyz; the moon is opposite). std140 offsets
    // 200, 204, 208.
    float specularStrength = 0.5f;
    float time = 0.0f;
    glm::vec4 sunDirection{0.0f, 1.0f, 0.0f, 0.0f};

    // Shadow receiving (see ShadowPass): three cascades, nearest first. Written
    // per frame through VulkanRenderEngine::SetShadows. std140 offsets 224
    // (matrices), 416, 432, 448, 464.
    glm::mat4 shadowViewProj[3] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
    glm::vec4 shadowParams{0.0f};   // x darkness, y one texel in uv, w debug view
    glm::vec4 shadowSplits{0.0f};   // x,y,z: far view distance of each cascade
    glm::vec4 shadowTexels{0.0f};   // x,y,z: blocks per texel of each cascade
    glm::vec4 shadowRanges{1.0f};   // x,y,z: depth span of each cascade, in blocks
};

} // namespace Volcano

#endif
