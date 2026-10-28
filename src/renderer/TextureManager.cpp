#include "TextureManager.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <algorithm>
#include <bit>
#include <optional>
#include <stdexcept>
#include <vector>
#include <cstring>
#include "VulkanInit.hpp"
#include "Logger.hpp"
#include "resources/ResourceManager.hpp"

namespace Volcano {

TextureManager::TextureManager() {
    //
}

TextureManager::~TextureManager() {
    //
}

void TextureManager::Destroy()
{
    if (array.sampler != VK_NULL_HANDLE) vkDestroySampler(GetDevice(), array.sampler, nullptr);
    if (array.view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), array.view, nullptr);
    if (array.image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, array.image, array.allocation);
    array = TextureArray{};
    layerLookup.clear();
    layerSize = 0;
    mipLevels = 0;
}

void TextureManager::CreateArrayImage(uint32_t layerCount)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { layerSize, layerSize, 1 };
    imageInfo.mipLevels = mipLevels;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // Source: Pincel reads icons back.
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.arrayLayers = layerCount;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(vmaAllocator, &imageInfo, &allocInfo, &array.image, &array.allocation, nullptr) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create texture array image!");
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = array.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = layerCount;

    vkCreateImageView(GetDevice(), &viewInfo, nullptr, &array.view);

    array.layerCount = layerCount;
}

void TextureManager::UploadLayer(uint32_t layerIndex, const uint8_t* rgbaPixels, uint32_t width, uint32_t height)
{
    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
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

namespace {

struct PackTexture
{
    std::string stem;
    std::vector<uint8_t> bytes;
    int width = 0;
    int frameHeight = 0; // Height of frame 0 (animation strips are stacked squares).
};

std::string StemOf(const std::string& logicalPath)
{
    size_t slash = logicalPath.find_last_of('/');
    std::string name = slash == std::string::npos ? logicalPath : logicalPath.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

} // namespace

void TextureManager::Reload(const std::function<void(float)>& onProgress)
{
    LoadResourcePack(onProgress);
}

void TextureManager::LoadResourcePack(const std::function<void(float)>& onProgress)
{
    Destroy();

    ResourceManager& resources = ResourceManager::Instance();

    // Later sources win a bare-stem collision (item over block), so the
    // shadowed texture never takes a layer it can't be looked up by.
    std::vector<PackTexture> textures;
    std::unordered_map<std::string, size_t> stemIndex;
    size_t found = 0;
    uint32_t skipped = 0;
    for (const char* subdir : { "assets/minecraft/textures/block", "assets/minecraft/textures/item" })
    {
        for (const std::string& path : resources.List(subdir, ".png", false))
        {
            found++;
            std::optional<std::vector<uint8_t>> bytes = resources.ReadBytes(path);
            int w = 0, h = 0, channels = 0;
            if (!bytes || !stbi_info_from_memory(bytes->data(), static_cast<int>(bytes->size()), &w, &h, &channels) || w <= 0 || h <= 0)
            {
                skipped++;
                continue;
            }

            PackTexture texture;
            texture.stem = StemOf(path);
            texture.bytes = std::move(*bytes);
            texture.width = w;
            texture.frameHeight = (h > w && h % w == 0) ? w : h;

            auto existing = stemIndex.find(texture.stem);
            if (existing != stemIndex.end())
            {
                textures[existing->second] = std::move(texture);
                continue;
            }
            stemIndex[texture.stem] = textures.size();
            textures.push_back(std::move(texture));
        }
    }

    Log::Info("Found " + std::to_string(found) + " texture files");
    if (skipped > 0)
    {
        Log::Info("Skipped " + std::to_string(skipped) + " unreadable textures");
    }

    if (textures.empty())
    {
        Log::Error("No textures found! Cannot create texture array.");
        return;
    }

    // Layer size: largest frame dimension, power of two, clamped.
    uint32_t largest = TEXTURE_MIN_LAYER_SIZE;
    for (const PackTexture& texture : textures)
    {
        largest = std::max(largest, static_cast<uint32_t>(std::max(texture.width, texture.frameHeight)));
    }
    uint32_t size = std::bit_ceil(largest);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(GetPhysicalDevice(), &props);
    uint32_t maxSize = std::min(TEXTURE_MAX_LAYER_SIZE, props.limits.maxImageDimension2D);
    if (size > maxSize)
    {
        Log::Info("Largest texture needs " + std::to_string(size) + "px layers; clamping to " + std::to_string(maxSize));
        size = maxSize;
    }
    layerSize = size;
    mipLevels = static_cast<uint32_t>(std::bit_width(size));

    // maxImageArrayLayers can be as low as 2048; drop the overflow (items
    // load last) rather than build a broken image.
    uint32_t maxLayers = props.limits.maxImageArrayLayers;
    if (textures.size() > maxLayers)
    {
        Log::Error("Too many textures (" + std::to_string(textures.size()) + ") for maxImageArrayLayers "
            + std::to_string(maxLayers) + "; dropping the last " + std::to_string(textures.size() - maxLayers));
        textures.resize(maxLayers);
    }

    CreateArrayImage(static_cast<uint32_t>(textures.size()));

    std::vector<uint8_t> scaled(static_cast<size_t>(layerSize) * layerSize * 4);
    uint32_t layer = 0;
    for (PackTexture& texture : textures)
    {
        int w, h, channels;
        uint8_t* pixels = stbi_load_from_memory(texture.bytes.data(), static_cast<int>(texture.bytes.size()),
            &w, &h, &channels, STBI_rgb_alpha);
        if (!pixels)
        {
            Log::Error("Failed to decode texture: " + texture.stem);
            std::fill(scaled.begin(), scaled.end(), 0); // Keep the layer defined.
            UploadLayer(layer, scaled.data(), layerSize, layerSize);
            layer++;
            continue;
        }

        // Nearest-neighbour resample of frame 0 to the layer size.
        size_t frameHeight = static_cast<size_t>(texture.frameHeight);
        for (uint32_t y = 0; y < layerSize; y++)
        {
            size_t sy = static_cast<size_t>(y) * frameHeight / layerSize;
            for (uint32_t x = 0; x < layerSize; x++)
            {
                size_t sx = static_cast<size_t>(x) * w / layerSize;
                memcpy(&scaled[(static_cast<size_t>(y) * layerSize + x) * 4], &pixels[(sy * w + sx) * 4], 4);
            }
        }
        stbi_image_free(pixels);

        UploadLayer(layer, scaled.data(), layerSize, layerSize);
        layerLookup[texture.stem] = static_cast<uint16_t>(layer);
        texture.bytes = {};
        layer++;

        if (onProgress) onProgress(static_cast<float>(layer) / static_cast<float>(textures.size()));
    }

    GenerateMipmaps();
    CreateSampler();

    double megabytes = static_cast<double>(layerSize) * layerSize * 4 * layer * 4.0 / 3.0 / (1024.0 * 1024.0);
    Log::Info("Loaded " + std::to_string(layer) + " textures into array ("
        + std::to_string(layerSize) + "px, " + std::to_string(mipLevels) + " mips, ~"
        + std::to_string(static_cast<int>(megabytes)) + " MB).");
}

uint16_t TextureManager::GetLayerIndex(const std::string& textureName) const
{
    auto it = layerLookup.find(textureName);
    // return it != layerLookup.end() ? it->second : 0;
    if (it == layerLookup.end()) {
        Log::Error("Texture not found: " + textureName);
        return 0;
    }
    return it->second;
}

bool TextureManager::HasLayer(const std::string& textureName) const
{
    return layerLookup.contains(textureName);
}

void TextureManager::GenerateMipmaps()
{
    VkCommandBuffer cmd = BeginOneShotCommands();

    int32_t mipWidth = static_cast<int32_t>(layerSize);
    int32_t mipHeight = static_cast<int32_t>(layerSize);

    // Only mip 0 of each layer was uploaded to (and transitioned to
    // TRANSFER_DST_OPTIMAL) in UploadLayer; mips 1..N-1 are still
    // VK_IMAGE_LAYOUT_UNDEFINED. Walk the chain, blitting each mip down from
    // the previous one, so every level actually contains downsampled data
    // instead of uninitialized VRAM.
    for (uint32_t mip = 1; mip < mipLevels; mip++)
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
    // TRANSFER_DST_OPTIMAL) - move it to its final shader-readable layout too.
    VkImageMemoryBarrier lastBarrier{};
    lastBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    lastBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    lastBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    lastBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    lastBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    lastBarrier.image = array.image;
    lastBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevels - 1, 1, 0, array.layerCount };
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
    samplerInfo.maxLod = static_cast<float>(mipLevels);

    if (vkCreateSampler(GetDevice(), &samplerInfo, nullptr, &array.sampler) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create texture sampler!");
    }
}

} // namespace Volcano
