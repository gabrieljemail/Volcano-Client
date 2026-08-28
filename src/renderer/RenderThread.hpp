#pragma once
#ifndef RENDER_THREAD_H
#define RENDER_THREAD_H

#include <vulkan/vulkan.hpp>
#include <thread>
#include <stop_token>
#include <memory>
#include "../GlobalState.hpp"

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

    void ThreadEntry(stop_token stopToken);
    void RenderLoop(stop_token stopToken);
    void WaitForTargetFrame();
    void PollInputs();
    void DrawFrame();
    void AcquireImage();
    void RecordAndSubmitFrame();
    void PresentFrame();
};

}; // namespace Volcano

#endif