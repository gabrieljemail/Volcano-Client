#pragma once
#ifndef VOLCANO_PARTICLE_SPRITES_H
#define VOLCANO_PARTICLE_SPRITES_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Volcano {

// Where one particle texture is in the atlas: a layer of the array, and the part of it the texture covers.
struct ParticleSprite {
    uint32_t layer = 0;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    float scale = 1.0f;
    // Edge scale for the pack's resolution: vanilla's 8px particle art is the 1.0 the sizes are written for, so a 2x pack
    // draws at half the size to keep the same world size.
};

// The particle textures, packed one to a layer of an array, and which ones each type of particle uses (its definition,
// assets/minecraft/particles/<type>.json, lists them in order). Loads from the resource packs and needs no Vulkan:
// ParticlePass uploads Pixels().
class ParticleSprites {
public:
    static constexpr uint32_t LAYER_SIZE = 64; // Fits a 4x pack's 16px art or a 2x pack's explosion frames.

    // Returns false if no texture could be loaded.
    bool Load();

    // A type's sprites, in the order its definition lists them. Empty for a type with none.
    const std::vector<ParticleSprite>& Of(const std::string& type) const;

    uint32_t LayerCount() const { return layerCount; }

    // RGBA8: LayerCount() layers of LAYER_SIZE x LAYER_SIZE, each texture at the top left of its layer.
    const std::vector<uint8_t>& Pixels() const { return pixels; }

private:
    std::unordered_map<std::string, std::vector<ParticleSprite>> byType;
    std::vector<uint8_t> pixels;
    uint32_t layerCount = 0;
};

} // namespace Volcano

#endif
