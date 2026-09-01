#include "TextureManager.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <filesystem>
#include <vector>
#include <iostream>

namespace fs = std::filesystem;

namespace Volcano {

TextureManager::TextureManager() {
    //
}

TextureManager::~TextureManager() {
    //
}

void TextureManager::CreateArrayImage(uint32_t layerCount)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { TEXTURE_LAYER_SIZE, TEXTURE_LAYER_SIZE, 1 };
    imageInfo.mipLevels = TEXTURE_MIP_LEVELS;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.arrayLayers = layerCount; // ADD THIS — this is what was missing

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    vmaCreateImage(vmaAllocator, &imageInfo, &allocInfo, &array.image, &array.allocation, nullptr);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = array.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = TEXTURE_MIP_LEVELS;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = layerCount;

    vkCreateImageView(GetDevice(), &viewInfo, nullptr, &array.view);

    array.layerCount = layerCount;
}

void TextureManager::UploadLayer(uint32_t layerIndex, const uint8_t* rgbaPixels, uint32_t width, uint32_t height)
{
    VkDeviceSize size = width * height * 4;
    AllocatedBuffer staging = CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);

    void* mapped;
    vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
    memcpy(mapped, rgbaPixels, size);
    vmaUnmapMemory(vmaAllocator, staging.allocation);

    VkCommandBuffer cmd = BeginOneShotCommands(); // helper: allocate+begin a transient command buffer

    TransitionImageLayout(cmd, array.image, layerIndex, 0, 1,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layerIndex, 1 };
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cmd, staging.buffer, array.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    EndOneShotCommands(cmd); // submits + waits — fine for load-time, not per-frame

    vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation); // staging is one-shot, free immediately
}

void TextureManager::LoadResourcePack(const std::string& resourcePackRoot)
{
    std::vector<fs::path> texturePaths;
    for (const auto& subdir : { "block", "item" })
    {
        fs::path dir = resourcePackRoot + "/" + "assets/minecraft/textures" + "/" + subdir;
        if (!fs::exists(dir)) continue;
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.path().extension() == ".png")
                texturePaths.push_back(entry.path());
    }

    CreateArrayImage(static_cast<uint32_t>(texturePaths.size()));

    uint16_t layer = 0;
    for (const auto& path : texturePaths)
    {
        int w, h, channels;
        uint8_t* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (!pixels)
        {
            std::cerr << "[WARN] Failed to load texture: " << path << std::endl;
            continue;
        }

        // Animated texture: PNG height is a multiple of width (stacked frames).
        // Take frame 0 only for now — see animation note above.
        int frameHeight = (h > w && h % w == 0) ? w : h;
        if (frameHeight != static_cast<int>(TEXTURE_LAYER_SIZE) || w != static_cast<int>(TEXTURE_LAYER_SIZE))
        {
            std::cerr << "[WARN] Skipping non-" << TEXTURE_LAYER_SIZE << "x" << TEXTURE_LAYER_SIZE << " texture (mixed-res packs not yet supported): " << path << std::endl;
            stbi_image_free(pixels);
            continue;
        }

        UploadLayer(layer, pixels, w, frameHeight); // only frame 0's rows if animated
        layerLookup[path.stem().string()] = layer;
        layer++;

        stbi_image_free(pixels);
    }

    GenerateMipmaps();
    CreateSampler();

    std::cout << "[INFO] Loaded " << layer << " textures into array." << std::endl;
}

uint16_t TextureManager::GetLayerIndex(const std::string& textureName) const
{
    auto it = layerLookup.find(textureName);
    return it != layerLookup.end() ? it->second : 0; // layer 0 as "missing texture" fallback — consider a magenta/black checker instead
}

} // namespace Volcano
