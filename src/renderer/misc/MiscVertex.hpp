#pragma once
#ifndef VOLCANO_MISC_VERTEX_H
#define VOLCANO_MISC_VERTEX_H

#include <cstdint>
#include <array>
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

namespace Volcano {

// Vertex format for the non-cubic render pass (see NonCubicMesher and
// misc.vert/misc.frag). Unlike PackedVertex, this geometry (cross
// billboards, partial-volume boxes) needs sub-block-fraction positions,
// which don't fit PackedVertex's integer bitfield encoding — so position is
// a plain float vec3 here, with texture layer/light/biome-tint packed into
// one uint the same way PackedVertex packs its own word1.
struct MiscVertex {
    glm::vec3 position; // Chunk-local block-space position; the model matrix places the chunk in the world, same as terrain vertices.
    glm::vec2 uv;        // 0..1 per face — non-cube quads are never greedy-merged, so no tiling repeat is needed.
    // textureLayer (bits 0-15), blockLight (bits 16-19), skyLight (bits
    // 20-23), biomeTinted (bit 24) — same split and same reason as
    // PackedVertex's word1 (see terrain.frag's own comment on why the two
    // channels stay separate all the way to the shader).
    uint32_t packed;

    static uint32_t Pack(uint16_t textureLayer, uint8_t skyLight, uint8_t blockLight, bool biomeTinted) {
        return (static_cast<uint32_t>(textureLayer) & 0xFFFFu)
            | ((static_cast<uint32_t>(blockLight) & 0xFu) << 16)
            | ((static_cast<uint32_t>(skyLight) & 0xFu) << 20)
            | ((biomeTinted ? 1u : 0u) << 24);
    }

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(MiscVertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(MiscVertex, position);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(MiscVertex, uv);

        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32_UINT;
        attributeDescriptions[2].offset = offsetof(MiscVertex, packed);

        return attributeDescriptions;
    }
};

} // namespace Volcano

#endif
