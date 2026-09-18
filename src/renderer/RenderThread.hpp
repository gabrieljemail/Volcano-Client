#pragma once
#ifndef RENDER_THREAD_H
#define RENDER_THREAD_H

#include <vulkan/vulkan.hpp>
#include <chrono>
#include "../GlobalState.hpp"
#include "gui/GUIController.hpp"

using namespace std;

namespace Volcano {

// Runs on the main (GLFW-owning) thread now — see the tick-loop plan's
// thread-merge step. There used to be a real render thread here, separate
// from main()'s glfwPollEvents() loop, purely because input had to be
// *captured* on the GLFW thread but was *consumed* here; now that this
// loop runs directly in main(), RunFrame() below calls glfwPollEvents()
// itself (see PollInputs()) and there's no cross-thread input buffering or
// shutdown hand-off left to manage.
class RenderThread {
public:
    explicit RenderThread(Volcano::GlobalState* globalState);
    ~RenderThread();

    // One iteration of the frame loop — call this directly from main()'s
    // own while loop instead of starting a separate thread. Runs
    // WaitForTargetFrame -> UpdateDeltaTime -> PollInputs -> DrawFrame ->
    // EndFrame, the same sequence RenderLoop used to run once per thread
    // iteration.
    void RunFrame();

    // Tears down the GUI/ImGui Vulkan resources and waits for the GPU to
    // finish with them first — call once after main()'s loop exits, before
    // VulkanInit::Cleanup() destroys the device/window out from under
    // them. Previously done at the tail of the render thread's own
    // ThreadEntry once its loop exited; there's no separate thread to do
    // that hand-off through anymore, so main() just calls this directly.
    void Shutdown();

private:
    Volcano::GlobalState* state;

    // Timing variables:
    chrono::steady_clock::time_point frameStartTime;
    chrono::steady_clock::time_point nextFrameTarget;
    double targetFrameTime = 1000.0 / 60.0;
    double averageWorkTime = 0.5;
    uint32_t currentFrame = 0;
    uint32_t imageIndex = 0;
    uint32_t maxFramesInFlight = 2;
    
    // FPS tracking
    chrono::steady_clock::time_point lastFrameTime;

    // Elapsed time (seconds) since the previous frame, computed once per loop
    // iteration so both movement (in PollInputs) and the GUI's FPS counter
    // (in DrawFrame) use the exact same value instead of sampling the clock
    // twice and drifting apart.
    float frameDeltaTime = 0.0f;

    // F3 (see VolcanoClient.cpp's "ToggleWireframe" action) — a debug tool
    // for telling "this face has no geometry at all" apart from "geometry
    // exists but lost the depth/winding test," which fill-mode alone can't
    // distinguish. RenderThread-owned since it's only ever read/written
    // from this thread's own frame loop, unlike GlobalState's cross-thread
    // fields.
    bool wireframeMode = false;

    // Current FOV boost in degrees, eased toward its target (Graphics.
    // FOVEffects while sprinting/flying, 0 otherwise — see
    // RecordAndSubmitFrame) every frame rather than snapping, so the zoom-out
    // on starting/stopping a sprint is a smooth widen/narrow instead of a
    // jump cut. RenderThread-owned for the same reason wireframeMode is:
    // only this thread's own frame loop ever touches it.
    float currentFovOffset = 0.0f;

    void WaitForTargetFrame();
    void UpdateDeltaTime();
    void PollInputs();
    void DrawFrame();
    bool AcquireImage();
    void RecordAndSubmitFrame();
    void PresentFrame();
    void Cleanup();
};

}; // namespace Volcano

#endif