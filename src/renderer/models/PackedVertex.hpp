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

    static PackedVertex Pack(glm::uvec3 localPos, uint8_t normalIndex,
            uint8_t ao, uint8_t u, uint8_t v, uint16_t textureLayer,
            uint8_t blockLight, uint8_t skyLight)
    {
        PackedVertex vert{};
        vert.word0 = (localPos.x & 0x3Fu)
            | ((localPos.y & 0x3Fu) << 6)
            | ((localPos.z & 0x3Fu) << 12)
            | ((normalIndex & 0x7u) << 18)
            | ((ao & 0x3u) << 21);
        vert.word1 = (u & 0x1u)
            | ((v & 0x1u) << 1)
            | ((static_cast<uint32_t>(textureLayer) & 0x3FFFu) << 2)
            | ((blockLight & 0xFu) << 16)
            | ((skyLight & 0xFu) << 20);
        return vert;
    }

    // Matches `layout(location = 0) in uvec2 packedData` in terrain.vert:
    // both words are fetched together as a single uvec2 attribute.
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
        attributeDescriptions[0].format = VK_FORMAT_R32G32_UINT;
        attributeDescriptions[0].offset = 0;

        return attributeDescriptions;
    }
};

} // namespace Volcano

#endif
