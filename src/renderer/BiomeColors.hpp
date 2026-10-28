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
        "fern",
        "large_fern_bottom",
        "large_fern_top",
        "sugar_cane",
        "vine",
        "oak_leaves",
        "jungle_leaves",
        "acacia_leaves",
        "dark_oak_leaves",
        "mangrove_leaves",
    };
    return whitelist.contains(textureName);
}

// This is the same placeholder situation as BIOME_COLOR_PLAINS above.
inline constexpr glm::vec3 BIOME_COLOR_WATER = { 0x3F / 255.0f, 0x76 / 255.0f, 0xE4 / 255.0f };

// Three bits in both vertex formats. Spruce, birch and the lily pad have one fixed color in vanilla whatever the
// biome (tints.json "constant"); the rest follow the biome, which is still the plains color until the client reads
// biome data.
enum class BiomeTint : uint8_t
{
    None = 0,
    Foliage = 1,
    Water = 2,
    Grass = 3,
    Spruce = 4,
    Birch = 5,
    LilyPad = 6,
};

inline BiomeTint GetMiscTint(const std::string& textureName)
{
    if (textureName == "water_still" || textureName == "water_flow") return BiomeTint::Water;
    if (textureName == "spruce_leaves") return BiomeTint::Spruce;
    if (textureName == "birch_leaves") return BiomeTint::Birch;
    if (textureName == "lily_pad") return BiomeTint::LilyPad;
    if (textureName == "grass" || textureName == "short_grass"
        || textureName == "tall_grass" || textureName == "tall_grass_bottom"
        || textureName == "tall_grass_top" || textureName == "fern"
        || textureName == "large_fern_bottom" || textureName == "large_fern_top"
        || textureName == "sugar_cane") return BiomeTint::Grass;
    return IsBiomeTinted(textureName) ? BiomeTint::Foliage : BiomeTint::None;
}

// The tint of a terrain (full cube) texture, for PackedVertex.
inline uint8_t GetTerrainTint(const std::string& textureName)
{
    if (textureName == "spruce_leaves") return static_cast<uint8_t>(BiomeTint::Spruce);
    if (textureName == "birch_leaves") return static_cast<uint8_t>(BiomeTint::Birch);
    return IsBiomeTinted(textureName) ? static_cast<uint8_t>(BiomeTint::Foliage) : static_cast<uint8_t>(BiomeTint::None);
}

} // namespace Volcano

#endif
