#pragma once
#include <vulkan/vulkan_core.h>
#ifndef TEXTURE_MANAGER_H
#define TEXTURE_MANAGER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.hpp>
#include "VulkanInit.hpp"

namespace Volcano {

constexpr uint32_t TEXTURE_LAYER_SIZE = 16;
constexpr uint32_t TEXTURE_MIP_LEVELS = 5; // log2(16) + 1

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

    // Loads every PNG under resourcePackRoot/assets/minecraft/textures/{block,item}/,
    // uploads into a single 2D array image, and returns a name->layer lookup.
    // Static textures only for now — .mcmeta animation frames are ignored (frame 0 used).
    void LoadResourcePack(const std::string& resourcePackRoot);

    uint16_t GetLayerIndex(const std::string& textureName) const;

    TextureArray array;
private:
    std::unordered_map<std::string, uint16_t> layerLookup;

    void CreateArrayImage(uint32_t layerCount);
    void UploadLayer(uint32_t layerIndex, const uint8_t* rgbaPixels, uint32_t width, uint32_t height);
    void GenerateMipmaps();
    void CreateSampler();
};

} // namespace Volcano

#endif
