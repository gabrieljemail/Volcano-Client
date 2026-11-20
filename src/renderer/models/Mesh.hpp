#pragma once
#ifndef MESH_H
#define MESH_H

#include <cstdint>
#include <chrono>
#include <glm/glm.hpp>

namespace Volcano {

struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
    glm::vec2 uv;
};

// One drawable chunk-section mesh. Deliberately backend-neutral: the buffer
// fields are OPAQUE GPU HANDLES, not a Vulkan or D3D11 type - the active
// backend's SlabBuffer hands out its own buffer pointer (VkBuffer or
// ID3D11Buffer*) and each backend casts back to its own type at draw time.
// vertexOffset /
// indexOffset are BYTE offsets into those shared slab buffers (see
// SlabBuffer), so a mesh never owns its memory; the mesher gives the region
// back through SlabBuffer::Free when the mesh is replaced or removed.
struct Mesh {
    void* vertexBuffer = nullptr;
    uint64_t vertexOffset = 0;
    uint32_t vertexCount = 0;

    void* indexBuffer = nullptr;
    uint64_t indexOffset = 0;
    uint32_t indexCount = 0;

    // Exact byte sizes of the slab regions above (0 if not slab-allocated),
    // so the region can be handed back via SlabBuffer::Free when this mesh
    // is replaced or removed
    uint64_t vertexBytes = 0;
    uint64_t indexBytes = 0;

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
