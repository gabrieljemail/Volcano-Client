#pragma once
#ifndef RENDER_THREAD_H
#define RENDER_THREAD_H

#include <vulkan/vulkan.hpp>
#include <thread>
#include <stop_token>
#include <memory>
#include "../GlobalState.hpp"
#include "gui/GUIController.hpp"

using namespace std;

namespace Volcano {

class RenderThread {
public:
    explicit RenderThread(Volcano::GlobalState* globalState);
    ~RenderThread();
    void Start();

private:
    Volcano::GlobalState* state;
    jthread worker;

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