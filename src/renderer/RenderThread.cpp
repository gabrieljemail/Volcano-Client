#include <iostream>
#include <chrono>
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>
#include "RenderThread.hpp"
#include "VulkanInit.hpp"

using namespace std;

namespace Volcano {

RenderThread::RenderThread(Volcano::GlobalState* globalState) : state(globalState)
{}

RenderThread::~RenderThread()
{}

void RenderThread::Start()
{
    worker = jthread([this](stop_token st) {
        this->ThreadEntry(st);
    });
}

void RenderThread::ThreadEntry(stop_token stopToken)
{
    cout << "[INFO] Render thread created." << endl;

    try {
        Init(); // From VulkanInit.hpp.
        RenderLoop(stopToken);
    } catch (const exception& e) {
        cerr << "[ERROR] Render thread crashed during startup: " << e.what() << endl;
    }
}

void RenderThread::RenderLoop(stop_token stopToken)
{
    frameStartTime = chrono::steady_clock::now();
    nextFrameTarget = frameStartTime + chrono::microseconds(static_cast<long long>(targetFrameTime * 1000));

    while (!stopToken.stop_requested())
    {
        WaitForTargetFrame();
        PollInputs();
        DrawFrame();
    }
}

void RenderThread::WaitForTargetFrame()
{
    using namespace chrono;

    // Calculate time remaining until next target.
    auto now = steady_clock::now();
    auto remaining = duration_cast<microseconds>(nextFrameTarget - now).count();

    // Sleep for the majority of the frame time.
    // Add safety padding to make sure the scheduler doesn't make us oversleep.
    if (remaining > 2000)
    {
        this_thread::sleep_for(microseconds(remaining - 1500)); // Time remaining - 1.5ms.
    }

    // Spin-wait the final microseconds.
    auto workOffset = microseconds(static_cast<long long>(averageWorkTime * 1000));
    auto spinTarget = nextFrameTarget - workOffset;

    while (steady_clock::now() < spinTarget)
    {
        this_thread::yield();
    }
}

void RenderThread::PollInputs()
{
    glfwPollEvents();
}

void RenderThread::DrawFrame()
{
    // Time the current frame.
    auto workStart = chrono::steady_clock::now();

    AcquireImage();
    RecordAndSubmitFrame();
    PresentFrame();

    auto workEnd = chrono::steady_clock::now();

    // Schedule the next frame.
    double frameWorkTime = std::chrono::duration<double, std::milli>(workEnd - workStart).count();
    averageWorkTime = (averageWorkTime * 0.9) + (frameWorkTime * 0.1);
    nextFrameTarget += std::chrono::microseconds(static_cast<long long>(targetFrameTime * 1000));
}

void RenderThread::AcquireImage()
{
    // TODO: vkAcquireNextImageKHR.
}

void RenderThread::RecordAndSubmitFrame()
{
    // TODO: vkBeginCommandBuffer -> Draw -> vkQueueSubmit.
}

void RenderThread::PresentFrame()
{
    // TODO: vkQueuePresentKHR.
}

}