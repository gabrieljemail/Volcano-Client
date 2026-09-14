#include <iostream>
#include <chrono>
#include <array>
#include <algorithm>
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>
#include "RenderThread.hpp"
#include "VulkanInit.hpp"
#include "models/CameraUBO.hpp"
#include "models/Mesh.hpp"
#include "terrain/VisibleChunkController.hpp"
#include "entity/EntityRenderer.hpp"
#include "gui/GUIController.hpp"
#include "../TickLoop.hpp"

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

    lastFrameTime = chrono::steady_clock::now();
}

RenderThread::~RenderThread()
{}

void RenderThread::Start()
{
    worker = jthread([this](stop_token st) {
        this->ThreadEntry(st);
    });
}

void RenderThread::RequestStop()
{
    worker.request_stop();
}

bool RenderThread::HasStopped() const
{
    return finished.load();
}

void RenderThread::Stop()
{
    worker.request_stop();
    if (worker.joinable())
    {
        worker.join();
    }
}

void RenderThread::ThreadEntry(stop_token stopToken)
{
    cout << "[INFO] Render thread created." << endl;

    try {
        RenderLoop(stopToken);
    } catch (const exception& e) {
        cerr << "[ERROR] Render thread crashed during startup: " << e.what() << endl;
    }

    // The last submitted frame's command buffer may still be executing on
    // the GPU (fences are only waited on at the *start* of the next frame,
    // which never comes once the loop above has exited) and it references
    // ImGui's descriptor pool/pipeline. GUIController::Shutdown() destroys
    // those Vulkan objects outright, so without this wait it tears down
    // resources the GPU is still using — previously masked entirely by the
    // present-time deadlock this loop used to hit before ever reaching here.
    vkDeviceWaitIdle(GetDevice());

    // Cleanup GUI controller
    GUIController::Shutdown();

    finished.store(true);
}

void RenderThread::RenderLoop(stop_token stopToken)
{
    frameStartTime = chrono::steady_clock::now();
    nextFrameTarget = frameStartTime + chrono::microseconds(static_cast<long long>(targetFrameTime * 1000));

    while (!stopToken.stop_requested() && !state->shouldClose)
    {
        WaitForTargetFrame();
        UpdateDeltaTime();
        PollInputs();
        DrawFrame();
        // Both PollInputs (F11, TickLoop's jump check) and DrawFrame (via
        // GUIController::Update()'s Escape check) read this frame's
        // press/release edges above — only clear them now that both have
        // had their chance, not before.
        state->input->EndFrame();
    }

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

void RenderThread::UpdateDeltaTime()
{
    auto now = chrono::steady_clock::now();
    frameDeltaTime = chrono::duration<float>(now - lastFrameTime).count();
    lastFrameTime = now;
}

void RenderThread::PollInputs()
{
    // Bind the close button (and Alt+F4).
    if (glfwWindowShouldClose(state->window))
    {
        state->shouldClose = true;
    }

    // Process inputs.
    state->input->ProcessFrame();

    // F11 cycles window modes regardless of whether a Screen is open, same
    // as it would in most games.
    if (state->input->IsKeyPressed(GLFW_KEY_F11))
    {
        ToggleWindowMode();
    }

    if (state->input->WasActivated("ToggleWireframe"))
    {
        wireframeMode = !wireframeMode;
    }

    // While a Screen (connect screen, future pause menu, etc.) or the chat
    // input box is open, ImGui owns the mouse/keyboard — don't let them
    // also fly the camera or move the player underneath it.
    if (GUIController::IsScreenOpen() || GUIController::IsChatInputOpen()) return;

    // Mouse sensitivity is applied entirely at the axis level (see the
    // Camera.X/Y registration in VolcanoClient.cpp) — pass 1.0f here so
    // there's exactly one sensitivity knob, not two multiplied together.
    float dx = state->input->GetAxis("Camera.X");
    float dy = state->input->GetAxis("Camera.Y");
    state->player->camera.ApplyMouseDelta(dx, dy, 1.0f);

    // Gravity/collision/movement now live in TickLoop, run at a fixed 20Hz
    // regardless of render framerate — see the tick-loop plan.
    state->tickLoop->Advance(frameDeltaTime);
}

void RenderThread::DrawFrame()
{
    // Time the current frame.
    auto workStart = chrono::steady_clock::now();

    // Resize requests land here (set by VulkanInit's GLFW framebuffer-size
    // callback, running on the main thread) — this is the only thread
    // allowed to touch the swapchain, so recreation happens here, between
    // frames, rather than in the callback itself.
    if (state->framebufferResized.exchange(false))
    {
        RecreateSwapchain(state->pendingFramebufferWidth.load(), state->pendingFramebufferHeight.load());
    }

    // Update GUI with new frame
    GUIController::NewFrame();
    GUIController::Update(frameDeltaTime);

    if (AcquireImage())
    {
        RecordAndSubmitFrame();
        PresentFrame();
        NotifyFramePresented();
    }

    auto workEnd = chrono::steady_clock::now();

    // Schedule the next frame.
    double frameWorkTime = std::chrono::duration<double, std::milli>(workEnd - workStart).count();
    averageWorkTime = (averageWorkTime * 0.9) + (frameWorkTime * 0.1);
    nextFrameTarget += std::chrono::microseconds(static_cast<long long>(targetFrameTime * 1000));
}

bool RenderThread::AcquireImage()
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

    // The swapchain is stale (surface no longer matches — a resize/fullscreen
    // toggle that outran the framebuffer-size callback, or a minimized
    // window). Skip this frame; RecreateSwapchain runs at the top of the
    // next DrawFrame once a resize is flagged (see below for the case where
    // no callback fired at all).
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        state->framebufferResized = true;
        return false;
    }

    // Suboptimal still yields a presentable image; only VK_SUCCESS and
    // VK_SUBOPTIMAL_KHR are non-fatal per the Vulkan spec.
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        throw runtime_error("[ERROR] Failed to acquire swapchain image!");
    }

    // Reset the fence.
    vkResetFences(GetDevice(), 1, &inFlightFences[currentFrame]);
    return true;
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

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.52941f, 0.80784f, 0.92157f, 1.00000f}}; // Sky blue.
    clearValues[1].depthStencil = {1.0f, 0};

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wireframeMode ? wireframePipeline : graphicsPipeline);

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

    // Set up camera UBOs.
    // We do this before mesh binding to support culling in the future.
    CameraUBO ubo{
        state->player->camera.GetViewMatrix(state->tickLoop->GetRenderPosition()),
        [&] {
            float aspect = static_cast<float>(swapchainExtent.width) / swapchainExtent.height;
            float fov = static_cast<float>(std::get<uint32_t>(state->config->Get("Graphics.FOV", uint32_t{100})));
            glm::mat4 p = glm::perspective(glm::radians(fov), aspect, 0.05f, 1000.0f);
            p[1][1] *= -1.0f;
            return p;
        }()
    };
    memcpy(cameraUBOsMapped[currentFrame], &ubo, sizeof(ubo));

    // Bind texture descriptor set.
    VkDescriptorSet sets[] = { cameraSets[currentFrame], textureSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 2, sets, 0, nullptr);

    // Bind each visible mesh. Locked against the main thread's
    // DrainNetworkInbox, which appends to renderList as chunks stream in —
    // see the mutex's comment in GlobalState.hpp. Frustum culling runs
    // inside the lock (cheap: a handful of dot products per chunk) so the
    // filtered list doesn't outlive renderList's own Mesh storage.
    glm::mat4 viewProj = ubo.proj * ubo.view;
    {
        std::lock_guard<std::mutex> lock(state->renderListMutex);
        for (const Mesh* meshPtr : VisibleChunkController::GetVisibleMeshes(state->renderList, viewProj))
        {
            const Mesh& mesh = *meshPtr;
            VkDeviceSize offsets[] = {mesh.vertexOffset};
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertexBuffer, offsets);

            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &mesh.modelMatrix);

            if (mesh.indexCount > 0)
            {
                vkCmdBindIndexBuffer(cmd, mesh.indexBuffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
            } else
            {
                vkCmdDraw(cmd, mesh.vertexCount, 1, 0, 0);
            }
        }
    }

    // Entities are a separate batched pass sharing this same subpass/depth
    // attachment — see EntityRenderer's header for why draw order relative
    // to the chunk pass above doesn't affect depth-test correctness.
    glm::vec3 cameraWorldPosition = state->player->camera.GetEyePosition(state->tickLoop->GetRenderPosition());
    EntityRenderer::RecordDraw(cmd, currentFrame, state, ubo.view, ubo.proj, cameraWorldPosition);

    // Non-cubic pass: transparent full cubes (glass, slime, ice, ...),
    // partial-volume shapes (slabs, carpets, ...), and cross-shaped plants
    // (grass, flowers, ...) — see NonCubicMesher/VulkanInit's
    // nonCubicPipeline. Drawn last (after opaque terrain and entities) with
    // alpha blending and no depth write, so it composites over what's
    // already in the color buffer without needing draw-order sorting within
    // itself. Same frustum-culling/locking pattern as the chunk pass above.
    {
        std::lock_guard<std::mutex> lock(state->nonCubicRenderListMutex);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nonCubicPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 2, sets, 0, nullptr);

        for (const Mesh* meshPtr : VisibleChunkController::GetVisibleMeshes(state->nonCubicRenderList, viewProj))
        {
            const Mesh& mesh = *meshPtr;
            if (mesh.vertexCount == 0) continue;

            VkDeviceSize offsets[] = {mesh.vertexOffset};
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertexBuffer, offsets);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &mesh.modelMatrix);

            if (mesh.indexCount > 0)
            {
                vkCmdBindIndexBuffer(cmd, mesh.indexBuffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
            } else
            {
                vkCmdDraw(cmd, mesh.vertexCount, 1, 0, 0);
            }
        }
    }

    // Render ImGui GUI
    GUIController::Render(cmd);

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

    VkSemaphore waitSemaphores[] = {renderFinishedSemaphores[currentFrame]};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = waitSemaphores;

    VkSwapchainKHR swapchains[] = {GetSwapchain()};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    // Flip the buffer to the display.
    VkResult result = vkQueuePresentKHR(presentQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        state->framebufferResized = true;
    }
    else if (result != VK_SUCCESS)
    {
        throw runtime_error("[ERROR] Failed to present swapchain image!");
    }

    // Move to the next frame slot.
    currentFrame = (currentFrame + 1) % maxFramesInFlight;
}

void RenderThread::Cleanup()
{
    // Render thread cleanup - currently no specific resources to clean up
    // The main Vulkan resources are cleaned up in VulkanInit::Cleanup()
}

}
