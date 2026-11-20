#pragma once
#ifndef VOLCANO_GUI_PINCEL_RASTERIZER_VULKAN_H
#define VOLCANO_GUI_PINCEL_RASTERIZER_VULKAN_H

#include "PincelRasterizer.hpp"
#include <array>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "models/AllocatedBuffer.hpp"

namespace Volcano::GUI {

class VulkanPincelRasterizer final : public PincelRasterizer {
public:
    VulkanPincelRasterizer() = default;
    ~VulkanPincelRasterizer() override;

    bool Create(Engine::RenderEngine& engine) override;
    void Destroy() override;
    void Upload(const Pincel::DrawData& data) override;
    void Record(Engine::FrameContext& frame) override;
    uint64_t UploadedVersion() const override { return uploadedVersion; }

private:
    struct FrameBuffers {
        AllocatedBuffer vertexBuffer;
        AllocatedBuffer indexBuffer;
        void* vertexMapped = nullptr;
        void* indexMapped = nullptr;
        size_t vertexCapacity = 0;
        size_t indexCapacity = 0;
        uint32_t indexCount = 0;
        uint32_t backgroundFirstIndex = 0;
        uint32_t backgroundIndexCount = 0;
        uint32_t canvasWidth = 0;
        uint32_t canvasHeight = 0;
        std::vector<Pincel::DrawCommand> commands;
    };

    struct GpuTexture {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptors[2]{};
        uint32_t width = 0;
        uint32_t height = 0;
        bool hasContent = false;
    };

    static constexpr uint32_t FRAME_COUNT = 3;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE;
    VkSampler nearestSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    std::array<FrameBuffers, FRAME_COUNT> frames;
    uint32_t currentFrame = 0;
    std::unordered_map<uint64_t, GpuTexture> imageTextures;
    GpuTexture fontTexture;
    GpuTexture fallbackTexture;
    uint64_t fontTextureVersion = 0;
    uint64_t uploadedVersion = 0;
};

} // namespace Volcano::GUI

#endif
