#pragma once
#ifndef VOLCANO_GUI_PINCEL_SPRITE_H
#define VOLCANO_GUI_PINCEL_SPRITE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <Pincel/Element.hpp>

namespace Volcano::GUI {

// An image in CPU memory (RGBA8, straight alpha), for giving to Pincel. Pincel
// draws an image as a plain copy when it is shown at its own size, so scale a
// sprite to the size it is shown at first (Scaled / Resized) and add that.
struct Sprite {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool IsEmpty() const { return rgba.empty(); }

    // A pack image ("assets/...") or a file on disk. Empty if it can't be read.
    static Sprite Load(const std::string& path);
    // The same, kept for the rest of the run; see ClearCache().
    static const Sprite& Cached(const std::string& path);
    static void ClearCache();

    // One layer of a GPU image the GPU has finished with (in shader-read layout), copied back to memory.
    static Sprite FromGpuImage(VkImage image, uint32_t layer, uint32_t width, uint32_t height);

    Sprite Crop(int x, int y, int cropWidth, int cropHeight) const;
    // A region of the sprite, scaled to `art` pixels per vanilla texture pixel, whatever resolution the image
    // really is (a pack can have 2x textures). `logicalWidth` x `logicalHeight` is the image's size in vanilla
    // texture pixels, and the region (x, y, regionWidth, regionHeight) is in those same units.
    Sprite Region(int logicalWidth, int logicalHeight, int x, int y, int regionWidth, int regionHeight, int art) const;
    // Each pixel made factor x factor: crisp pixel art.
    Sprite ScaledNearest(int factor) const;
    // Smoothly resized, for shrinking a big image once instead of every frame.
    Sprite Resized(int newWidth, int newHeight) const;

    // Adds an Image of this at its own size, drawn as a plain copy.
    std::optional<Pincel::Element> AddTo(Pincel::Element parent) const;
    // Replaces an Image's pixels with this.
    bool SetOn(Pincel::Element image) const;
};

} // namespace Volcano::GUI

#endif
