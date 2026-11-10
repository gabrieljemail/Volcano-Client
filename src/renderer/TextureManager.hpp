#pragma once
#include <vulkan/vulkan_core.h>
#ifndef TEXTURE_MANAGER_H
#define TEXTURE_MANAGER_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include "VulkanInit.hpp"

namespace Volcano {

// The actual layer size is derived from the loaded pack (TextureManager::LayerSize()).
constexpr uint32_t TEXTURE_MIN_LAYER_SIZE = 16;
constexpr uint32_t TEXTURE_MAX_LAYER_SIZE = 128; // Larger textures are downscaled (nearest).

struct TextureArray {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t layerCount = 0;
};

class TextureManager {
public:
    TextureManager();
    ~TextureManager();

    // Loads every PNG under assets/minecraft/textures/{block,item} as resolved
    // by ResourceManager and uploads into a single 2D array image. Layer size
    // is the largest texture (power of two, min 16); smaller textures are
    // upscaled nearest-neighbour. Animation strips use frame 0.
    // Safe to call again: any previous array is destroyed first (see Reload).
    // onProgress, if given, is called with 0..1 as textures are uploaded.
    void LoadResourcePack(const std::function<void(float)>& onProgress = nullptr);

    // Rebuilds the atlas in place. The caller must have stopped rendering and
    // called vkDeviceWaitIdle, then re-attach array.view/sampler to its
    // descriptor sets and refresh every cached layer index afterwards.
    void Reload(const std::function<void(float)>& onProgress = nullptr);

    // Frees the texture array. Safe to call repeatedly or before a load. Must
    // run after rendering stops and before the VMA allocator is destroyed.
    void Destroy();

    // Edge length in pixels of every layer, and mip count (0 before a load).
    uint32_t LayerSize() const { return layerSize; }
    uint32_t MipLevels() const { return mipLevels; }

    uint16_t GetLayerIndex(const std::string& textureName) const;

    // Same lookup as GetLayerIndex, without its "not found" log line
    bool HasLayer(const std::string& textureName) const;

    TextureArray array;
private:
    std::unordered_map<std::string, uint16_t> layerLookup;
    uint32_t layerSize = 0;
    uint32_t mipLevels = 0;

    void CreateArrayImage(uint32_t layerCount);
    void UploadLayer(uint32_t layerIndex, const uint8_t* rgbaPixels, uint32_t width, uint32_t height);
    void GenerateMipmaps();
    void CreateSampler();
};

} // namespace Volcano

#endif
