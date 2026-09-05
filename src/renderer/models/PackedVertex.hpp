#pragma once
#ifndef PACKED_VERTEX_H
#define PACKED_VERTEX_H

#include <cstdint>
#include <array>
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

namespace Volcano {

struct PackedVertex {
    uint32_t word0;
    uint32_t word1;
    uint32_t word2;

    static PackedVertex Pack(glm::uvec3 localPos, uint8_t normalIndex,
            uint8_t ao, uint8_t u, uint8_t v, uint16_t textureLayer,
            uint8_t blockLight, uint8_t skyLight)
    {
        PackedVertex vert{};
        // word0: position (6+6+6=18 bits), normalIndex (3 bits), ao (2 bits), padding (9 bits)
        vert.word0 = (localPos.x & 0x3Fu)
            | ((localPos.y & 0x3Fu) << 6)
            | ((localPos.z & 0x3Fu) << 12)
            | ((normalIndex & 0x7u) << 18)
            | ((ao & 0x3u) << 21);
        
        // word1: textureLayer (16 bits), blockLight (4 bits), skyLight (4 bits), padding (8 bits)
        vert.word1 = (static_cast<uint32_t>(textureLayer) & 0xFFFFu)
            | ((blockLight & 0xFu) << 16)
            | ((skyLight & 0xFu) << 20);
        
        // word2: u (8 bits), v (8 bits), padding (16 bits)
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
