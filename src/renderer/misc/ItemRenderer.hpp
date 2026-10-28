#pragma once
#ifndef VOLCANO_ITEM_RENDERER_H
#define VOLCANO_ITEM_RENDERER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
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

    // True if the item has a drawable 3D model.
    bool HasModel(const std::string& itemName) const;

    // The pipeline for drawing into the main render pass (dropped items, the
    // held item). Built by the pass that uses it, once the render pass exists.
    void CreateWorldPipeline(const Engine::ShaderLibrary& shaders);
    void DestroyWorldPipeline();
    // Builds the world pipeline on first use; passes call it each frame.
    void EnsureWorldPipeline(const Engine::ShaderLibrary& shaders)
    {
        if (worldPipeline == VK_NULL_HANDLE) CreateWorldPipeline(shaders);
    }

    // Draws an item inside the main pass: `placement` * the model's display
    // transform for `context` ("ground", "firstperson_righthand", ...), then
    // `viewProj`. Needs viewport/scissor already set by the caller.
    // `pivotRotation` turns the item in place about its own center, in
    // `placement` space, after the display transform.
    void RecordItem(VkCommandBuffer cmd, const std::string& itemName, const std::string& context,
                    const glm::mat4& placement, const glm::mat4& viewProj,
                    const glm::mat4& pivotRotation = glm::mat4(1.0f));

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
    VkPipeline worldPipeline = VK_NULL_HANDLE; // Same shaders, targeting the main render pass.

    // Shared depth buffer for the icon passes (each pass clears it).
    VkImage depthImage = VK_NULL_HANDLE;
    VmaAllocation depthAllocation = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    std::unordered_map<std::string, GpuMesh> meshes;

    void CreateRenderPass();
    void CreateDepthBuffer();
    VkPipeline BuildPipeline(const Engine::ShaderLibrary& shaders, VkRenderPass pass);
    GpuBuffer UploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    const GpuMesh* MeshFor(const std::string& itemName, const ItemModel& model);
};

} // namespace Volcano

#endif
