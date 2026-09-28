#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include "VulkanRenderEngine.hpp"
#include "../passes/EntityPass.hpp"
#include "../passes/GUIPass.hpp"
#include "../passes/NonCubicPass.hpp"
#include "../passes/SelectionPass.hpp"
#include "../passes/ShaderPackPass.hpp"
#include "../passes/TerrainPass.hpp"
#include "../../VulkanInit.hpp"
#include "../../TextureManager.hpp"
#include "../../models/CameraUBO.hpp"
#include "../../gui/GUIController.hpp"
#include "../../../GlobalState.hpp"
#include "../../../Logger.hpp"

namespace Volcano::Engine::Vulkan {

void VulkanRenderEngine::Init()
{
    // Window, instance, device, swapchain, render pass, descriptor sets,
    // command buffers, sync objects — everything but pipelines.
    ::Init(state);
    initialized = true;

    shaders.SetActivePack(std::get<std::string>(state->config->Get("Graphics.ShaderPack", std::string(""))));

    RegisterBuiltInPasses();
    CreatePasses();

    Log::Info(std::string("[INFO] Render engine initialized (") + GetBackendName() + ", "
        + std::to_string(passes.size()) + " passes).");
}

void VulkanRenderEngine::RegisterBuiltInPasses()
{
    AddPass(std::make_unique<Passes::TerrainPass>());
    AddPass(std::make_unique<Passes::EntityPass>());
    AddPass(std::make_unique<Passes::NonCubicPass>());
    AddPass(std::make_unique<Passes::SelectionPass>());
    AddPass(std::make_unique<Passes::ShaderPackPass>());
    AddPass(std::make_unique<Passes::GUIPass>());
}

void VulkanRenderEngine::AttachTextures(const TextureManager& textures)
{
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = textures.array.view;
    imageInfo.sampler = textures.array.sampler;

    VkWriteDescriptorSet textureWrite{};
    textureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    textureWrite.dstSet = GetTextureSet();
    textureWrite.dstBinding = 0;
    textureWrite.descriptorCount = 1;
    textureWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureWrite.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(GetDevice(), 1, &textureWrite, 0, nullptr);
}

void VulkanRenderEngine::AttachGUI(const TextureManager* textures)
{
    GUIController::Init(state->window, GetRenderPass(), 2, state, textures); // 2 is the swapchain image count
    guiAttached = true;
}

void VulkanRenderEngine::StopRendering()
{
    // The last submitted frame's command buffer may still be executing on
    // the GPU (fences are only waited on at the *start* of the next frame,
    // which never comes once the frame loop has exited) and it references
    // ImGui's descriptor pool/pipeline and every pass's pipelines — all
    // destroyed outright below.
    WaitIdle();

    if (guiAttached)
    {
        GUIController::Shutdown();
        guiAttached = false;
    }
    DestroyPasses();
}

void VulkanRenderEngine::Shutdown()
{
    // Normally already done by StopRendering(); harmless if so.
    if (passesCreated) StopRendering();

    ::Cleanup();
    initialized = false;
}

bool VulkanRenderEngine::BeginFrame()
{
    // Resize requests land here (set by VulkanInit's GLFW framebuffer-size
    // callback) — this is the only thread allowed to touch the swapchain,
    // so recreation happens here, between frames, rather than in the
    // callback itself.
    if (state->framebufferResized.exchange(false))
    {
        RecreateSwapchain(state->pendingFramebufferWidth.load(), state->pendingFramebufferHeight.load());
    }

    // Between frames is also the only safe point to rebuild pipelines.
    if (shaderReloadRequested.exchange(false))
    {
        ReloadPassShaders();
    }

    // Have the CPU wait until this frame slot's previous submission is done.
    vkWaitForFences(GetDevice(), 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);

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
    // next BeginFrame now that a resize is flagged.
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        state->framebufferResized = true;
        return false;
    }

    // Suboptimal still yields a presentable image; only VK_SUCCESS and
    // VK_SUBOPTIMAL_KHR are non-fatal per the Vulkan spec.
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        throw std::runtime_error("[ERROR] Failed to acquire swapchain image!");
    }

    // Only reset once we know this slot will actually be submitted —
    // resetting before a skipped frame would leave the next wait hanging.
    vkResetFences(GetDevice(), 1, &inFlightFences[currentFrame]);
    return true;
}

void VulkanRenderEngine::SubmitFrame(const SceneView& scene)
{
    VkCommandBuffer cmd = commandBuffers[currentFrame];

    BeginCommandBuffer(cmd);
    UploadCamera(scene);
    BeginMainPass(cmd, scene);

    FrameContext frame;
    frame.engine = this;
    frame.state = state;
    frame.scene = &scene;
    frame.frameIndex = currentFrame;
    frame.extent = GetExtent();
    frame.commandBuffer = cmd;
    frame.viewProj = scene.proj * scene.view;
    RecordPasses(frame);

    EndMainPass(cmd);
    SubmitCommandBuffer(cmd);
    PresentImage();
    NotifyFramePresented();

    currentFrame = (currentFrame + 1) % maxFramesInFlight;
}

RenderExtent VulkanRenderEngine::GetExtent() const
{
    return RenderExtent{ swapchainExtent.width, swapchainExtent.height };
}

void VulkanRenderEngine::WaitIdle()
{
    if (initialized) vkDeviceWaitIdle(GetDevice());
}

void VulkanRenderEngine::ToggleWindowMode()
{
    ::ToggleWindowMode();
}

void VulkanRenderEngine::BeginCommandBuffer(VkCommandBuffer cmd)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);
}

void VulkanRenderEngine::UploadCamera(const SceneView& scene)
{
    CameraUBO ubo{ scene.view, scene.proj, scene.lightingEnabled ? 1.0f : 0.0f };
    std::memcpy(cameraUBOsMapped[currentFrame], &ubo, sizeof(ubo));
}

void VulkanRenderEngine::BeginMainPass(VkCommandBuffer cmd, const SceneView& scene)
{
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{scene.skyColor.r, scene.skyColor.g, scene.skyColor.b, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = framebuffers[imageIndex];
    renderPassInfo.renderArea.extent = swapchainExtent;
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Every pipeline uses dynamic viewport/scissor — set once, here, for
    // all passes.
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
}

void VulkanRenderEngine::EndMainPass(VkCommandBuffer cmd)
{
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

void VulkanRenderEngine::SubmitCommandBuffer(VkCommandBuffer cmd)
{
    // Wait for the acquired image before writing color; signal
    // renderFinished for the present, and this slot's fence for the CPU.
    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[currentFrame]};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to submit semaphore to graphics queue.");
    }
}

void VulkanRenderEngine::PresentImage()
{
    VkSemaphore waitSemaphores[] = {renderFinishedSemaphores[currentFrame]};
    VkSwapchainKHR swapchains[] = {GetSwapchain()};

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = waitSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    VkResult result = vkQueuePresentKHR(presentQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        state->framebufferResized = true;
    }
    else if (result != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to present swapchain image!");
    }
}

} // namespace Volcano::Engine::Vulkan
