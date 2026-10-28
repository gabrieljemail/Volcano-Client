#pragma once
#ifndef PACKED_VERTEX_H
#define PACKED_VERTEX_H

#include <cstdint>
#include <array>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace Volcano {

struct PackedVertex {
    uint32_t word0;
    uint32_t word1;
    uint32_t word2;

    static PackedVertex Pack(glm::uvec3 localPos, uint8_t normalIndex,
            uint8_t ao, uint8_t u, uint8_t v, uint16_t textureLayer,
            uint8_t blockLight, uint8_t skyLight, uint8_t biomeTint = 0, uint8_t shininess = 0)
    {
        PackedVertex vert{};
        // word0: x (6 bits, 0-15), z (6 bits, 0-15), y (9 bits - section-
        // relative 0-15 for terrain, since ChunkMesher now meshes one
        // 16-tall section at a time; the field stayed 9 bits, wide enough
        // for the old 0-383 whole-column range too, rather than shrinking
        // it for a few unused bits), normalIndex (3 bits), ao (2 bits),
        // biomeTint (3 bits - BiomeColors.hpp's BiomeTint), padding (3 bits). World Y comes from the
        // mesh's own model matrix, which now translates to the section's
        // own Y origin instead of always WORLD_MIN_Y.
        vert.word0 = (localPos.x & 0x3Fu)
            | ((localPos.z & 0x3Fu) << 6)
            | ((localPos.y & 0x1FFu) << 12)
            | ((normalIndex & 0x7u) << 21)
            | ((ao & 0x3u) << 24)
            | ((biomeTint & 0x7u) << 26);

        // word1: textureLayer (16 bits), blockLight (4 bits), skyLight (4 bits), shininess (3 bits), padding (5 bits)
        vert.word1 = (static_cast<uint32_t>(textureLayer) & 0xFFFFu)
            | ((blockLight & 0xFu) << 16)
            | ((skyLight & 0xFu) << 20)
            | ((shininess & 0x7u) << 24);

        // word2: u (8 bits), v (8 bits), padding (16 bits)
        // u/v are raw block-space tile counts (not normalized 0-1) - see
        // terrain.vert, which relies on this for greedy-meshed quads to tile
        // their texture instead of stretching across the merged rect.
        vert.word2 = (static_cast<uint32_t>(u) & 0xFFu)
            | ((static_cast<uint32_t>(v) & 0xFFu) << 8);

        return vert;
    }

    // 12-byte stride (3 x uint32_t) - optimized for iGPU cache lines
    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(PackedVertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 1> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 1> attributeDescriptions{};

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_UINT;
        attributeDescriptions[0].offset = 0;

        return attributeDescriptions;
    }
};

} // namespace Volcano

#endif
