#pragma once
#ifndef MESH_H
#define MESH_H

#include <cstdint>
#include <chrono>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>

namespace Volcano {

struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
    glm::vec2 uv;

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, position);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, color);

        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(Vertex, uv);

        return attributeDescriptions;
    }
};

struct Mesh {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceSize vertexOffset = 0;
    uint32_t vertexCount = 0;

    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    uint32_t indexCount = 0;

    // Exact byte sizes of the slab regions above (0 if not slab-allocated),
    // so the region can be handed back via SlabBuffer::Free when this mesh
    // is replaced or removed
    VkDeviceSize vertexBytes = 0;
    VkDeviceSize indexBytes = 0;

    glm::mat4 modelMatrix = glm::mat4(1.0f);
    std::chrono::steady_clock::time_point fadeStart{};

    // The mesher had geometry but couldn't get slab space for it. Not a mesh
    // to draw (every count is 0): the caller must keep whatever mesh the
    // section already has and retry later, rather than treat it as "nothing
    // to draw here" and erase it.
    bool allocationFailed = false;
};

}; // namespace Volcano

#endif
