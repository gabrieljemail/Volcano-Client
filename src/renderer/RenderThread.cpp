#include <iostream>
#include <chrono>
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>
#include "RenderThread.hpp"
#include "VulkanInit.hpp"

using namespace std;

namespace Volcano {

RenderThread::RenderThread(Volcano::GlobalState* globalState) : state(globalState)
{
    if (state && state->targetFPS > 0)
    {
        targetFrameTime = 1000.0 / static_cast<double>(state->targetFPS);
    } else
    {
        targetFrameTime = 1000.0 / 60.0;
    }
}

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
        RenderLoop(stopToken);
    } catch (const exception& e) {
        cerr << "[ERROR] Render thread crashed during startup: " << e.what() << endl;
    }
}

void RenderThread::RenderLoop(stop_token stopToken)
{
    frameStartTime = chrono::steady_clock::now();
    nextFrameTarget = frameStartTime + chrono::microseconds(static_cast<long long>(targetFrameTime * 1000));

    while (!stopToken.stop_requested() && !state->shouldClose)
    {
        WaitForTargetFrame();
        PollInputs();
        DrawFrame();
    }

    Cleanup();
    // When the render loop exits for some reason, tell the main thread to shut down.
    state->shouldClose = true;
    // And then continue to the destructor to join the thread.
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
    // Bind the close button (and Alt+F4).
    if (glfwWindowShouldClose(window))
    {
        state->shouldClose = true;
    }

    // Process inputs.
    // TODO: Do something with inputs.
}

void RenderThread::DrawFrame()
{
    // Time the current frame.
    auto workStart = chrono::steady_clock::now();

    AcquireImage();
    RecordAndSubmitFrame();
    vkWaitForFences(GetDevice(), 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
    PresentFrame();

    auto workEnd = chrono::steady_clock::now();

    // Schedule the next frame.
    double frameWorkTime = std::chrono::duration<double, std::milli>(workEnd - workStart).count();
    averageWorkTime = (averageWorkTime * 0.9) + (frameWorkTime * 0.1);
    nextFrameTarget += std::chrono::microseconds(static_cast<long long>(targetFrameTime * 1000));
}

void RenderThread::AcquireImage()
{
    // Have the CPU wait for fences.
    vkWaitForFences(GetDevice(), 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);

    // Acquire the next image's index.
    VkResult result = vkAcquireNextImageKHR(
        GetDevice(),
        GetSwapchain(),
        UINT64_MAX,
        imageAvailableSemaphores[currentFrame],
        VK_NULL_HANDLE,
        &imageIndex
    );

    if (result != VK_SUCCESS)
    {
        throw runtime_error("[ERROR] Failed to acquire swapchain image!");
    }

    // Reset the fence.
    vkResetFences(GetDevice(), 1, &inFlightFences[currentFrame]);
}

void RenderThread::RecordAndSubmitFrame()
{
    VkCommandBuffer cmd = commandBuffers[currentFrame];

    // Recording.
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Clear the screen.
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = framebuffers[imageIndex];
    renderPassInfo.renderArea.extent = swapchainExtent;

    VkClearValue clearColor = {{{0.0f, 0.0f, 1.0f, 1.0f}}};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

    // Set the dynamic viewport.
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainExtent.width);
    viewport.height = static_cast<float>(swapchainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // TODO: Draw something real.
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    // Submit the frame.
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    // Wait for the swapchain to begin rendering.
    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    // When rendering is complete, have the GPU signal this semaphore.
    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[currentFrame]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    // Set up a GPU signal to the semaphore when done.
    VkResult queueResult = vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]);
    if (queueResult != VK_SUCCESS)
    {
        throw runtime_error("[ERROR] Failed to submit semaphore to graphics queue.");
    }
}

void RenderThread::PresentFrame()
{
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

    VkSwapchainKHR swapchains[] = {GetSwapchain()};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    // Flip the buffer to the display.
    vkQueuePresentKHR(presentQueue, &presentInfo);

    // Move to the next frame slot.
    currentFrame = (currentFrame + 1) % maxFramesInFlight; // 0 -> 1 -> 2 -> 0.
}

}