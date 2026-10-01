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
#include "../passes/SkyboxPass.hpp"
#include "../passes/TerrainPass.hpp"
#include "../../VulkanInit.hpp"
#include "../../TextureManager.hpp"
#include "../../models/CameraUBO.hpp"
#include "../../models/SlabBuffer.hpp"
#include "../../Screenshot.hpp"
#include <utility>
#include <vector>
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
    hizBuildPass.Create(shaders);

    Log::Info(std::string("[INFO] Render engine initialized (") + GetBackendName() + ", "
        + std::to_string(passes.size()) + " passes).");
}

void VulkanRenderEngine::RegisterBuiltInPasses()
{
    AddPass(std::make_unique<Passes::SkyboxPass>());
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
    hizBuildPass.Destroy();
    DestroyPasses();

    if (screenshotBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, screenshotBuffer.buffer, screenshotBuffer.allocation);
        screenshotBuffer = AllocatedBuffer{};
    }
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
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
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

    // This slot's fence was waited in BeginFrame, so its Hi-Z readback from
    // a couple of frames ago is done — publish it before the chunk passes
    // cull against it.
    hizBuildPass.BeginFrame(currentFrame);
    // Past this slot's fence wait: lets SlabBuffer reuse regions freed
    // enough frames ago (see SlabBuffer::FREE_DELAY_FRAMES).
    renderFrameCounter++;

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
    bool capture = screenshotRequested.exchange(false);
    if (capture) RecordScreenshotCopy(cmd);
    // Outside the render pass instance now (compute can't run inside one) —
    // see HiZBuildPass's own comment on why it isn't a normal RenderPass.
    hizBuildPass.Record(cmd, currentFrame, frame.viewProj);
    vkEndCommandBuffer(cmd);
    SubmitCommandBuffer(cmd);
    PresentImage();
    NotifyFramePresented();
    if (capture) FinishScreenshot();

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

void VulkanRenderEngine::ApplyWindowSettings()
{
    ::ApplyWindowSettings();
}

void VulkanRenderEngine::BeginCommandBuffer(VkCommandBuffer cmd)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);
}

void VulkanRenderEngine::UploadCamera(const SceneView& scene)
{
    CameraUBO ubo{ scene.view, scene.proj, scene.lightingEnabled ? 1.0f : 0.0f,
        scene.brightnessFactor, scene.dayBrightness, scene.lightFalloff,
        scene.shadeTop, scene.shadeBottom, scene.shadeNorthSouth, scene.shadeEastWest,
        glm::vec4(scene.skyColor, 1.0f), glm::vec4(scene.cameraPosition, 1.0f), scene.fogStart, scene.fogEnd };
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
    // vkEndCommandBuffer is NOT here: SubmitFrame still records HiZBuildPass
    // after the render pass ends — see its own vkEndCommandBuffer call.
}

void VulkanRenderEngine::RecordScreenshotCopy(VkCommandBuffer cmd)
{
    VkDeviceSize size = static_cast<VkDeviceSize>(swapchainExtent.width) * swapchainExtent.height * 4;
    if (screenshotBuffer.buffer != VK_NULL_HANDLE && screenshotBuffer.info.size < size)
    {
        vmaDestroyBuffer(vmaAllocator, screenshotBuffer.buffer, screenshotBuffer.allocation);
        screenshotBuffer = AllocatedBuffer{};
    }
    if (screenshotBuffer.buffer == VK_NULL_HANDLE)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &screenshotBuffer.buffer,
                &screenshotBuffer.allocation, &screenshotBuffer.info) != VK_SUCCESS)
        {
            Log::Error("Screenshot failed: couldn't allocate a readback buffer.");
            screenshotBuffer = AllocatedBuffer{};
            return;
        }
    }
    screenshotExtent = swapchainExtent;

    std::vector<VkImage> images = swapchain.get_images().value();
    VkImage image = images[imageIndex];

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { swapchainExtent.width, swapchainExtent.height, 1 };
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screenshotBuffer.buffer, 1, &region);

    // Back to present, and make the copy visible to the host read.
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &hostBarrier, 0, nullptr, 0, nullptr);
}

void VulkanRenderEngine::FinishScreenshot()
{
    if (screenshotBuffer.buffer == VK_NULL_HANDLE) return;

    // A one-frame stall on F2 is fine.
    vkWaitForFences(GetDevice(), 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
    vmaInvalidateAllocation(vmaAllocator, screenshotBuffer.allocation, 0, VK_WHOLE_SIZE);

    size_t pixelCount = static_cast<size_t>(screenshotExtent.width) * screenshotExtent.height;
    std::vector<uint8_t> rgba(pixelCount * 4);
    std::memcpy(rgba.data(), screenshotBuffer.info.pMappedData, rgba.size());

    // Most Windows swapchains are BGRA; PNG wants RGBA. Alpha is forced
    // opaque (the swapchain's alpha isn't meaningful).
    VkFormat format = swapchain.image_format;
    bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < pixelCount; i++)
    {
        if (bgra) std::swap(rgba[i * 4], rgba[i * 4 + 2]);
        rgba[i * 4 + 3] = 255;
    }

    Screenshot::SaveAsync(std::move(rgba), screenshotExtent.width, screenshotExtent.height,
        state->config->ResolvePath("screenshots"));
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
    // Losing exclusive fullscreen (alt-tab, etc.) just means rebuilding.
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR
        || result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
    {
        state->framebufferResized = true;
    }
    else if (result != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to present swapchain image!");
    }
}

} // namespace Volcano::Engine::Vulkan
