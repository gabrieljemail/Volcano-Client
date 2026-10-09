#pragma once
#ifndef VOLCANO_ITEM_RENDERER_H
#define VOLCANO_ITEM_RENDERER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "../engine/ShaderLibrary.hpp"

namespace Volcano {

class ItemModel;
class TextureManager;

// Draws ItemModels with the block/item texture array. Right now it renders
// one thing: inventory/hotbar icons, each drawn once into a small offscreen
// image with the model's "gui" display transform, then reused as a plain 2D
// texture. The same pipeline and cached GPU meshes are what the hand and
// dropped-item draws will use.
//
// Not thread-safe: render thread only (it submits one-shot command buffers).
class ItemRenderer {
public:
    static constexpr uint32_t GUI_ICON_SIZE = 64;

    // A finished icon. The caller (GUIController) owns it: it registers the
    // view with ImGui and destroys image + view on shutdown.
    struct IconImage {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    explicit ItemRenderer(const TextureManager* textureManager) : textures(textureManager) {}

    // Needs VulkanInit's device/command pool and the texture array already
    // attached (RenderEngine::AttachTextures).
    void Init(const Engine::ShaderLibrary& shaders);
    // Must run before the VMA allocator is destroyed.
    void Shutdown();

    // Renders the item's icon. Returns false (and leaves `out` null) if the
    // item has no 3D model, so the caller can fall back to its 2D texture.
    bool RenderGuiIcon(const std::string& itemName, IconImage& out);

private:
    struct GpuBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
    };

    // A model's vertex/index buffers, uploaded once and kept.
    struct GpuMesh {
        GpuBuffer vertices;
        GpuBuffer indices;
        uint32_t indexCount = 0;
    };

    const TextureManager* textures;
    bool initialized = false;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    // Shared depth buffer for the icon passes (each pass clears it).
    VkImage depthImage = VK_NULL_HANDLE;
    VmaAllocation depthAllocation = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    std::unordered_map<std::string, GpuMesh> meshes;

    void CreateRenderPass();
    void CreateDepthBuffer();
    void CreatePipeline(const Engine::ShaderLibrary& shaders);
    GpuBuffer UploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    const GpuMesh* MeshFor(const std::string& itemName, const ItemModel& model);
};

} // namespace Volcano

#endif
