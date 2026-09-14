#pragma once
#ifndef RENDER_THREAD_H
#define RENDER_THREAD_H

#include <vulkan/vulkan.hpp>
#include <thread>
#include <stop_token>
#include <memory>
#include <atomic>
#include "../GlobalState.hpp"
#include "gui/GUIController.hpp"

using namespace std;

namespace Volcano {

class RenderThread {
public:
    explicit RenderThread(Volcano::GlobalState* globalState);
    ~RenderThread();
    void Start();
    // Signals the render loop to exit without waiting for it. The caller
    // (the GLFW-owning main thread) must keep pumping glfwPollEvents() —
    // via HasStopped() below — until the thread actually finishes, rather
    // than blocking in Stop()/join() with no message pump running: on
    // Windows, the render thread's last vkQueuePresentKHR can depend on the
    // window's message queue being serviced, and a thread blocked in join()
    // services no messages, which deadlocks the driver instead of raising a
    // TDR (observed as a permanently frozen display on shutdown).
    void RequestStop();
    // True once the render thread has finished RenderLoop and its own
    // GUIController::Shutdown() — i.e. it has stopped touching Vulkan/GLFW
    // and it's safe to stop pumping messages and join.
    bool HasStopped() const;
    // Blocks until the render thread has fully exited. Must be called
    // before VulkanInit::Cleanup() destroys the device/window out from
    // under it — jthread's own destructor does this too, but only once the
    // RenderThread object itself is destroyed, which on a normal shutdown
    // happens after main() has already returned. Call RequestStop() and
    // poll HasStopped() (while still pumping messages) first; by the time
    // HasStopped() is true this just joins the (already finished) thread.
    void Stop();

private:
    Volcano::GlobalState* state;
    jthread worker;
    std::atomic<bool> finished{false};

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

    void ThreadEntry(stop_token stopToken);
    void RenderLoop(stop_token stopToken);
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