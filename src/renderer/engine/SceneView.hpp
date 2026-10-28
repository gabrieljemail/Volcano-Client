#pragma once
#ifndef VOLCANO_ENGINE_SCENE_VIEW_H
#define VOLCANO_ENGINE_SCENE_VIEW_H

#include <glm/glm.hpp>

namespace Volcano::Engine {

// Everything the client decides about a frame, handed to the engine in one
// piece - RenderThread builds this from camera/config/world state (FOV
// easing, day/night sky, debug toggles) and never touches the backend's own
// objects to do it. Plain glm/POD only, so nothing on the client side of
// the RenderEngine boundary needs a graphics API header to fill it in.
struct SceneView {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};

    // Eye position in world space
    glm::vec3 cameraPosition{0.0f};

    // The walk's sway, already in `view` (identity without view bobbing). The
    // held hand applies it too, since it is drawn with a projection of its own.
    glm::mat4 viewBob{1.0f};

    // Clear color for the main pass (already gamma-corrected by
    // Graphics.Brightness
    glm::vec3 skyColor{0.0f};
    float dayBrightness = 1.0f;

    // Vanilla's Brightness slider (0=Moody, 1=Bright) and the light curve's
    // falloff (Graphics.Brightness / Graphics.LightFalloff).
    float brightnessFactor = 0.5f;
    float lightFalloff = 3.0f;

    // Sky gradient (zenith -> horizon; skyColor above is the horizon/fog
    // color), sun direction (the moon is opposite), moon phase 0-7 (vanilla
    // order, 0 = full), and fog range in blocks.
    glm::vec3 skyTopColor{0.0f};
    glm::vec3 sunDirection{0.0f, 1.0f, 0.0f};
    int moonPhase = 0;
    float fogStart = 0.0f;
    float fogEnd = 0.0f;

    // Vanilla's per-face diffuse shading (Graphics.Shading.*), forwarded
    // into CameraUBO
    float shadeTop = 1.0f;
    float shadeBottom = 0.4f;
    float shadeNorthSouth = 0.8f;
    float shadeEastWest = 0.6f;

    // Terrain specular strength, 0 (off) to 1 (Graphics.Specular).
    float specularStrength = 0.5f;

    // Seconds since the client started, and since the previous frame -
    // mostly for shader pack overlays that animate.
    float time = 0.0f;
    float deltaTime = 0.0f;

    // True while the client isn't showing a world yet (main menu, connecting,
    // loading chunks): SkyboxPass draws the rotating title panorama instead
    // of leaving the plain sky-colored clear.
    bool menuBackground = false;

    // Graphics.Lighting ("G") and the F3+Z wireframe debug view.
    bool lightingEnabled = true;
    bool wireframe = false;

    // Graphics.IndirectDraw: draw chunk sections with one multi-draw indirect
    // call per list instead of a draw per section. Ignored (per-section draws)
    // where the device or shader pack can't do it
    bool indirectDraw = true;
};

} // namespace Volcano::Engine

#endif
