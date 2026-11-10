#pragma once
#ifndef RENDER_THREAD_H
#define RENDER_THREAD_H

#include <chrono>
#include <glm/glm.hpp>
#include "../GlobalState.hpp"
#include "engine/RenderEngine.hpp"
#include "engine/SceneView.hpp"

using namespace std;

namespace Volcano {

// Runs on the main (GLFW-owning) thread now
class RenderThread {
public:
    explicit RenderThread(Volcano::GlobalState* globalState);
    ~RenderThread();

    // One iteration of the frame loop
    void RunFrame();

    // Stops the engine rendering (waits for the GPU, tears down the GUI and
    // every render pass) - call once after main()'s loop exits, before
    // RenderEngine::Shutdown() destroys the device/window out from under
    // them.
    void Shutdown();

private:
    Volcano::GlobalState* state;

    // Timing variables:
    chrono::steady_clock::time_point frameStartTime;
    chrono::steady_clock::time_point nextFrameTarget;
    double targetFrameTime = 1000.0 / 60.0;
    double averageWorkTime = 0.5;

    // SceneView::time is measured from here.
    chrono::steady_clock::time_point startTime;

    // FPS tracking
    chrono::steady_clock::time_point lastFrameTime;

    // Elapsed time (seconds) since the previous frame, computed once per loop
    // iteration so both movement (in PollInputs) and the GUI's FPS counter
    // (in DrawFrame) use the exact same value instead of sampling the clock
    // twice and drifting apart.
    float frameDeltaTime = 0.0f;

    // F3+Z - a debug tool for telling "this face has no geometry at all"
    // apart from "geometry exists but lost the depth/winding test," which
    // fill-mode alone can't distinguish. RenderThread-owned since it's only
    // ever read/written from this thread's own frame loop, unlike
    // GlobalState's cross-thread fields.
    bool wireframeMode = false;

    // Set when a key was pressed while F3 was held, so releasing F3 doesn't
    // also toggle the debug HUD (see PollInputs).
    bool f3ComboUsed = false;

    // F3+T: run at the top of the next DrawFrame, before any ImGui or command
    // buffer work starts. See ResourceReloader::Reload.
    bool reloadResourcesRequested = false;

    // Latched on/off by a Sprint press when Controls.ToggleSprint is set -
    // see PollInputs. RenderThread-owned for the same reason wireframeMode
    // is; irrelevant (never read) while Controls.ToggleSprint is off, since
    // Sprint is then read as a plain held key instead.
    bool sprintToggled = false;

    // Current FOV boost in degrees, eased toward its target (Graphics.
    // FOVEffects while sprinting/flying, 0 otherwise
    float currentFovOffset = 0.0f;

    void WaitForTargetFrame();
    void UpdateDeltaTime();
    void PollInputs();
    void DrawFrame();

    // Everything the engine needs to know about this frame
    Engine::SceneView BuildSceneView();
    glm::mat4 BuildProjection(const Engine::RenderExtent& extent);
};

}; // namespace Volcano

#endif
