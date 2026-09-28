#pragma once
#ifndef VOLCANO_ENGINE_SCENE_VIEW_H
#define VOLCANO_ENGINE_SCENE_VIEW_H

#include <glm/glm.hpp>

namespace Volcano::Engine {

// Everything the client decides about a frame, handed to the engine in one
// piece — RenderThread builds this from camera/config/world state (FOV
// easing, day/night sky, debug toggles) and never touches the backend's own
// objects to do it. Plain glm/POD only, so nothing on the client side of
// the RenderEngine boundary needs a graphics API header to fill it in.
struct SceneView {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};

    // Eye position in world space — the same point view was built from,
    // passed separately so passes that sort or cull by distance (non-cubic,
    // entities) don't have to invert the view matrix to recover it.
    glm::vec3 cameraPosition{0.0f};

    // Clear color for the main pass, and the 0 (midnight) .. 1 (noon)
    // brightness it was mixed from — see RenderThread's DayBrightness().
    glm::vec3 skyColor{0.0f};
    float dayBrightness = 1.0f;

    // Seconds since the client started, and since the previous frame —
    // mostly for shader pack overlays that animate.
    float time = 0.0f;
    float deltaTime = 0.0f;

    // Graphics.Lighting ("G") and the F3 wireframe debug view.
    bool lightingEnabled = true;
    bool wireframe = false;
};

} // namespace Volcano::Engine

#endif
