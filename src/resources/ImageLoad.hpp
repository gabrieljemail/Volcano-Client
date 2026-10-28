#pragma once
#ifndef VOLCANO_IMAGE_LOAD_H
#define VOLCANO_IMAGE_LOAD_H

#include <stb_image.h>
#include "ResourceManager.hpp"

namespace Volcano {

// Decodes a pack image (logical path, e.g. "assets/minecraft/textures/...")
// to RGBA8. Free the result with stbi_image_free; nullptr if missing or bad.
inline stbi_uc* LoadImageRGBA(const std::string& logicalPath, int& width, int& height)
{
    auto bytes = ResourceManager::Instance().ReadBytes(logicalPath);
    if (!bytes) return nullptr;

    int channels = 0;
    return stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()),
                                 &width, &height, &channels, STBI_rgb_alpha);
}

} // namespace Volcano

#endif
