#include "TextureManager.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <filesystem>
#include <vector>
#include <cstring>
#include "VulkanInit.hpp"
#include "Logger.hpp"

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
    imageInfo.arrayLayers = layerCount;

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

    VkCommandBuffer cmd = BeginOneShotCommands();

    TransitionImageLayout(cmd, array.image, 0, 1, layerIndex, 1,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layerIndex, 1 };
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cmd, staging.buffer, array.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    EndOneShotCommands(cmd);

    vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);
}

void TextureManager::LoadResourcePack(const std::string& resourcePackRoot)
{
    std::vector<fs::path> texturePaths;
    for (const auto& subdir : { "block", "item" })
    {
        fs::path dir = fs::path(resourcePackRoot) / "assets" / "minecraft" / "textures" / subdir;
        if (!fs::exists(dir)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.path().extension() == ".png")
                texturePaths.push_back(entry.path());
    }

    Log::Info("[INFO] Found " + std::to_string(texturePaths.size()) + " texture files");

    if (texturePaths.empty()) {
        Log::Error("[ERROR] No textures found! Cannot create texture array.");
        return;
    }

    // CreateArrayImage's layerCount must match the number of textures that
    // will actually be uploaded, not the raw file count: this driver's
    // maxImageArrayLayers is 2048, and the raw file count (including
    // non-16x16 textures that get skipped below) can exceed that, silently
    // producing a broken image (vmaCreateImage's result was never checked).
    // Filter up front so the array is sized to what's really going in.
    std::vector<fs::path> validPaths;
    uint16_t skipped = 0;
    for (const auto& path : texturePaths)
    {
        int w, h, channels;
        if (!stbi_info(path.string().c_str(), &w, &h, &channels))
        {
            skipped++;
            continue;
        }

        // Animated texture: PNG height is a multiple of width (stacked frames).
        int frameHeight = (h > w && h % w == 0) ? w : h;
        if (frameHeight != static_cast<int>(TEXTURE_LAYER_SIZE) || w != static_cast<int>(TEXTURE_LAYER_SIZE))
        {
            skipped++;
            continue;
        }

        validPaths.push_back(path);
    }

    if (skipped > 0) {
        Log::Info("[WARN] Skipped " + std::to_string(skipped) + " non-16x16 textures");
    }

    CreateArrayImage(static_cast<uint32_t>(validPaths.size()));

    uint16_t layer = 0;
    for (const auto& path : validPaths)
    {
        int w, h, channels;
        uint8_t* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (!pixels)
        {
            Log::Error("[WARN] Failed to load texture: " + path.string());
            continue;
        }

        int frameHeight = (h > w && h % w == 0) ? w : h;
        UploadLayer(layer, pixels, w, frameHeight); // only frame 0's rows if animated
        layerLookup[path.stem().string()] = layer;
        layer++;

        stbi_image_free(pixels);
    }

    GenerateMipmaps();
    CreateSampler();

    Log::Info("[INFO] Loaded " + std::to_string(layer) + " textures into array.");
}

uint16_t TextureManager::GetLayerIndex(const std::string& textureName) const
{
    auto it = layerLookup.find(textureName);
    // return it != layerLookup.end() ? it->second : 0;
    if (it == layerLookup.end()) {
        Log::Error("[WARN] Texture not found: " + textureName);
        return 0;
    }
    return it->second;
}

void TextureManager::GenerateMipmaps()
{
    VkCommandBuffer cmd = BeginOneShotCommands();

    int32_t mipWidth = static_cast<int32_t>(TEXTURE_LAYER_SIZE);
    int32_t mipHeight = static_cast<int32_t>(TEXTURE_LAYER_SIZE);

    // Only mip 0 of each layer was uploaded to (and transitioned to
    // TRANSFER_DST_OPTIMAL) in UploadLayer; mips 1..N-1 are still
    // VK_IMAGE_LAYOUT_UNDEFINED. Walk the chain, blitting each mip down from
    // the previous one, so every level actually contains downsampled data
    // instead of uninitialized VRAM.
    for (uint32_t mip = 1; mip < TEXTURE_MIP_LEVELS; mip++)
    {
        VkImageMemoryBarrier toSrcBarrier{};
        toSrcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSrcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrcBarrier.image = array.image;
        toSrcBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1, 0, array.layerCount };
        toSrcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toSrcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toSrcBarrier);

        VkImageMemoryBarrier toDstBarrier{};
        toDstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDstBarrier.image = array.image;
        toDstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, array.layerCount };
        toDstBarrier.srcAccessMask = 0;
        toDstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toDstBarrier);

        int32_t nextWidth = mipWidth > 1 ? mipWidth / 2 : 1;
        int32_t nextHeight = mipHeight > 1 ? mipHeight / 2 : 1;

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, array.layerCount };
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {nextWidth, nextHeight, 1};
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, array.layerCount };
        vkCmdBlitImage(cmd,
            array.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            array.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        VkImageMemoryBarrier toReadBarrier{};
        toReadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toReadBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toReadBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toReadBarrier.image = array.image;
        toReadBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1, 0, array.layerCount };
        toReadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toReadBarrier);

        mipWidth = nextWidth;
        mipHeight = nextHeight;
    }

    // The last mip level was only ever a blit destination (still
    // TRANSFER_DST_OPTIMAL) — move it to its final shader-readable layout too.
    VkImageMemoryBarrier lastBarrier{};
    lastBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    lastBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    lastBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    lastBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    lastBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    lastBarrier.image = array.image;
    lastBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, TEXTURE_MIP_LEVELS - 1, 1, 0, array.layerCount };
    lastBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    lastBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &lastBarrier);

    EndOneShotCommands(cmd);
}

void TextureManager::CreateSampler()
{
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(TEXTURE_MIP_LEVELS);

    if (vkCreateSampler(GetDevice(), &samplerInfo, nullptr, &array.sampler) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create texture sampler!");
    }
}

} // namespace Volcano
