#include "PincelSprite.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#include "../VulkanInit.hpp"
#include "../../Logger.hpp"
#include "../../resources/ImageLoad.hpp"

namespace Volcano::GUI {

namespace {

std::unordered_map<std::string, Sprite>& Cache()
{
    static std::unordered_map<std::string, Sprite> cache;
    return cache;
}

} // namespace

Sprite Sprite::Load(const std::string& path)
{
    int width = 0, height = 0, channels = 0;
    uint8_t* pixels = path.starts_with("assets/")
        ? LoadImageRGBA(path, width, height)
        : stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);

    Sprite sprite;
    if (pixels == nullptr)
    {
        Log::Error("Sprite: failed to load " + path);
        return sprite;
    }
    sprite.width = width;
    sprite.height = height;
    sprite.rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return sprite;
}

const Sprite& Sprite::Cached(const std::string& path)
{
    auto& cache = Cache();
    auto found = cache.find(path);
    if (found == cache.end()) found = cache.emplace(path, Load(path)).first;
    return found->second;
}

void Sprite::ClearCache()
{
    Cache().clear();
}

Sprite Sprite::FromGpuImage(VkImage image, uint32_t layer, uint32_t width, uint32_t height)
{
    Sprite sprite;
    if (image == VK_NULL_HANDLE || width == 0 || height == 0) return sprite;

    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
    AllocatedBuffer staging = CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    if (staging.buffer == VK_NULL_HANDLE) return sprite;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 };

    VkCommandBuffer cmd = BeginOneShotCommands();

    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
    region.imageExtent = { width, height, 1 };
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier);

    EndOneShotCommands(cmd); // Waits for the copy.

    void* mapped = nullptr;
    if (vmaMapMemory(vmaAllocator, staging.allocation, &mapped) == VK_SUCCESS)
    {
        sprite.width = static_cast<int>(width);
        sprite.height = static_cast<int>(height);
        sprite.rgba.assign(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + size);
        vmaUnmapMemory(vmaAllocator, staging.allocation);
    }
    vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);
    return sprite;
}

Sprite Sprite::Crop(int x, int y, int cropWidth, int cropHeight) const
{
    Sprite cropped;
    x = std::clamp(x, 0, width);
    y = std::clamp(y, 0, height);
    cropWidth = std::clamp(cropWidth, 0, width - x);
    cropHeight = std::clamp(cropHeight, 0, height - y);
    if (cropWidth == 0 || cropHeight == 0) return cropped;

    cropped.width = cropWidth;
    cropped.height = cropHeight;
    cropped.rgba.resize(static_cast<size_t>(cropWidth) * cropHeight * 4);
    for (int row = 0; row < cropHeight; row++)
    {
        const uint8_t* from = rgba.data() + (static_cast<size_t>(y + row) * width + x) * 4;
        std::memcpy(cropped.rgba.data() + static_cast<size_t>(row) * cropWidth * 4, from, static_cast<size_t>(cropWidth) * 4);
    }
    return cropped;
}

Sprite Sprite::Region(int logicalWidth, int logicalHeight, int x, int y, int regionWidth, int regionHeight, int art) const
{
    if (IsEmpty() || logicalWidth <= 0 || logicalHeight <= 0) return Sprite{};

    // From vanilla texture pixels to this image's own.
    float scaleX = static_cast<float>(width) / static_cast<float>(logicalWidth);
    float scaleY = static_cast<float>(height) / static_cast<float>(logicalHeight);
    Sprite part = Crop(static_cast<int>(std::lround(x * scaleX)), static_cast<int>(std::lround(y * scaleY)),
        static_cast<int>(std::lround(regionWidth * scaleX)), static_cast<int>(std::lround(regionHeight * scaleY)));
    if (part.IsEmpty()) return part;

    int targetWidth = regionWidth * art;
    int targetHeight = regionHeight * art;
    if (part.width == targetWidth && part.height == targetHeight) return part;
    if (targetWidth % part.width == 0 && targetHeight % part.height == 0 && targetWidth / part.width == targetHeight / part.height)
    {
        return part.ScaledNearest(targetWidth / part.width); // Pixel art enlarged in whole steps stays crisp.
    }
    return part.Resized(targetWidth, targetHeight);
}

Sprite Sprite::ScaledNearest(int factor) const
{
    if (factor <= 1 || IsEmpty()) return *this;

    Sprite scaled;
    scaled.width = width * factor;
    scaled.height = height * factor;
    scaled.rgba.resize(static_cast<size_t>(scaled.width) * scaled.height * 4);
    for (int y = 0; y < scaled.height; y++)
    {
        for (int x = 0; x < scaled.width; x++)
        {
            std::memcpy(scaled.rgba.data() + (static_cast<size_t>(y) * scaled.width + x) * 4,
                rgba.data() + (static_cast<size_t>(y / factor) * width + x / factor) * 4, 4);
        }
    }
    return scaled;
}

Sprite Sprite::Resized(int newWidth, int newHeight) const
{
    if (IsEmpty() || newWidth <= 0 || newHeight <= 0) return Sprite{};
    if (newWidth == width && newHeight == height) return *this;

    Sprite resized;
    resized.width = newWidth;
    resized.height = newHeight;
    resized.rgba.resize(static_cast<size_t>(newWidth) * newHeight * 4);
    if (stbir_resize_uint8_srgb(rgba.data(), width, height, 0, resized.rgba.data(), newWidth, newHeight, 0, STBIR_RGBA) == nullptr)
    {
        return Sprite{};
    }
    return resized;
}

std::optional<Pincel::Element> Sprite::AddTo(Pincel::Element parent) const
{
    if (IsEmpty()) return std::nullopt;
    auto image = parent.AddImage(rgba.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    if (image) image->SetImageFilter(Pincel::ImageFilter::Nearest);
    return image;
}

bool Sprite::SetOn(Pincel::Element image) const
{
    return !IsEmpty() && image.SetImage(rgba.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

} // namespace Volcano::GUI
