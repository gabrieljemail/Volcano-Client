#pragma once
#ifndef BIOME_COLORS_H
#define BIOME_COLORS_H

#include <cstdint>
#include <string>
#include <unordered_set>
#include <glm/glm.hpp>

namespace Volcano {

// TODO: Get this from the server.
inline constexpr glm::vec3 BIOME_COLOR_PLAINS = { 145.0f / 255.0f, 189.0f / 255.0f, 89.0f / 255.0f };

// Textures whose color should be multiplied by the current biome color
// (grass top, foliage, etc.) rather than rendered at their raw (grayscale)
// resource-pack color.
inline bool IsBiomeTinted(const std::string& textureName)
{
    static const std::unordered_set<std::string> whitelist = {
        "grass_block_top",
        "grass_block_side_overlay",
        "grass",
        "short_grass",
        "tall_grass",
        "tall_grass_bottom",
        "tall_grass_top",
        "oak_leaves",
        "spruce_leaves",
        "birch_leaves",
        "jungle_leaves",
        "dark_oak_leaves",
        "mangrove_leaves",
        "pale_oak_leaves",
        "azalea_leaves",
        "flowering_azalea_leaves",
    };
    return whitelist.contains(textureName);
}

// This is the same placeholder situation as BIOME_COLOR_PLAINS above.
inline constexpr glm::vec3 BIOME_COLOR_WATER = { 0x3F / 255.0f, 0x76 / 255.0f, 0xE4 / 255.0f };

enum class BiomeTint : uint8_t
{
    None = 0,
    Foliage = 1,
    Water = 2,
    Grass = 3,
};

inline BiomeTint GetMiscTint(const std::string& textureName)
{
    if (textureName == "water_still") return BiomeTint::Water;
    if (textureName == "grass" || textureName == "short_grass"
        || textureName == "tall_grass" || textureName == "tall_grass_bottom"
        || textureName == "tall_grass_top") return BiomeTint::Grass;
    return IsBiomeTinted(textureName) ? BiomeTint::Foliage : BiomeTint::None;
}

} // namespace Volcano

#endif
