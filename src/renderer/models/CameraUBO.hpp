#pragma once
#ifndef CAMERA_UBO_H
#define CAMERA_UBO_H

#include <cstdint>
#include <glm/glm.hpp>

namespace Volcano {

struct CameraUBO {
    glm::mat4 view;
    glm::mat4 proj;
    // 1.0 = shade terrain/non-cube geometry by per-block light as normal,
    // 0.0 = force full-bright (Graphics.Lighting off, toggled by "G" — see
    // RenderThread::PollInputs). A float rather than bool so it round-trips
    // into GLSL's std140 layout without a bool-size mismatch.
    float lightingEnabled = 1.0f;
};

} // namespace Volcano

#endif
