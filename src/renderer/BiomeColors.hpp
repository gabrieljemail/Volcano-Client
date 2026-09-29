#pragma once
#ifndef BIOME_COLORS_H
#define BIOME_COLORS_H

#include <cstdint>
#include <string>
#include <unordered_set>
#include <glm/glm.hpp>

namespace Volcano {

// Placeholder biome-tint slot for the vertical slice: real biome tinting
// needs per-block biome data from the server (grass/foliage color varies by
// biome temperature/humidity), which doesn't exist yet. Until then, every
// whitelisted texture is tinted with a single hardcoded "plains" color so the
// packing/shader plumbing is already in place — swapping this constant for a
// real per-block lookup later shouldn't require touching the vertex format
// or shaders again.
inline constexpr glm::vec3 BIOME_COLOR_PLAINS = { 145.0f / 255.0f, 189.0f / 255.0f, 89.0f / 255.0f };

// Textures whose color should be multiplied by the current biome color
// (grass top, foliage, etc.) rather than rendered at their raw (grayscale)
// resource-pack color.
inline bool IsBiomeTinted(const std::string& textureName)
{
    static const std::unordered_set<std::string> whitelist = {
        "grass_block_top",
        "oak_leaves",
    };
    return whitelist.contains(textureName);
}

// Same placeholder situation as BIOME_COLOR_PLAINS above — vanilla's default
// (swamp-less) water color, since water_still.png is grayscale and relies
// entirely on a biome-color multiply to read as blue.
inline constexpr glm::vec3 BIOME_COLOR_WATER = { 0x3F / 255.0f, 0x76 / 255.0f, 0xE4 / 255.0f };

// Which placeholder biome color (if any) a texture should be tinted with —
// widened from IsBiomeTinted's plain bool because the non-cubic render pass
// (MiscVertex/misc.frag) now has two colors to choose between, not one.
// ChunkMesher/PackedVertex/terrain.frag are untouched: full-cube opaque
// blocks (grass, leaves) still only ever need IsBiomeTinted's single bool.
enum class BiomeTint : uint8_t
{
    None = 0,
    Foliage = 1,
    Water = 2,
};

inline BiomeTint GetMiscTint(const std::string& textureName)
{
    if (textureName == "water_still") return BiomeTint::Water;
    return IsBiomeTinted(textureName) ? BiomeTint::Foliage : BiomeTint::None;
}

} // namespace Volcano

#endif
