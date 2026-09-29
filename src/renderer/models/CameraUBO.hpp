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

    // Vanilla's Brightness slider (0=Moody, 1=Bright) — see terrain.frag's NotGamma.
    float brightnessFactor = 0.5f;

    // Sky-light channel's day/night multiplier (0 at midnight, 1 at noon) —
    // block light stays constant, matching vanilla.
    float skyLightFactor = 1.0f;

    // Shapes LightCurve's mid-range falloff — see terrain.frag. 0 = linear.
    float lightCurveBias = 5.0f;

    // Vanilla's hardcoded per-face diffuse shading (LevelRenderer::getShade)
    // — a flat multiplier by which cardinal direction a face points, giving
    // terrain a "lit from above" look independent of the real light level.
    // Configurable via Graphics.Shading.* rather than hardcoded in the
    // shader so a shader pack (or a future "flat lighting" toggle) can
    // override it. Indexed in terrain.frag by the packed vertex's
    // normalIndex — see ChunkMesher's FaceIndexFor for the Up/Down/North/
    // South/East/West -> 0-5 mapping this must match (North and South share
    // shadeNorthSouth, East and West share shadeEastWest, same as vanilla).
    float shadeTop = 1.0f;
    float shadeBottom = 0.5f;
    float shadeNorthSouth = 0.8f;
    float shadeEastWest = 0.6f;
};

} // namespace Volcano

#endif
