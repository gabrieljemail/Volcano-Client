#include "ChunkMesher.hpp"
#include "models/PackedVertex.hpp"
#include "../models/SlabBuffer.hpp"
#include "../VulkanInit.hpp"
#include <glm/ext/matrix_transform.hpp>
#include <vector>
#include <cstring>
#include <iostream>

namespace Volcano {

// Chunk meshes are streamed in continuously as the world loads, so rather than
// creating one tiny VkBuffer/VmaAllocation per chunk (which is slow and puts a
// lot of pressure on the allocator/driver), vertex and index data is bump-
// allocated out of a pair of large, persistently-mapped slab buffers.
static SlabBuffer vertexSlab;
static SlabBuffer indexSlab;
static bool slabsInitialized = false;

static void EnsureSlabsInitialized() {
    if (slabsInitialized) return;

    vertexSlab.Init(ChunkMesher::DEFAULT_SLAB_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    indexSlab.Init(ChunkMesher::DEFAULT_SLAB_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    slabsInitialized = true;
}

void ChunkMesher::Shutdown() {
    if (!slabsInitialized) return;

    vertexSlab.Destroy();
    indexSlab.Destroy();
    slabsInitialized = false;
}

// Faces: 0=Up, 1=Down, 2=North(-Z), 3=South(+Z), 4=East(+X), 5=West(-X)
static const glm::vec3 voxelVertices[6][4] = {
    { {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 0.0f} }, // Up
    { {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f} }, // Down
    { {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f} }, // North
    { {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 1.0f} }, // South
    { {1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f} }, // East
    { {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f} }  // West
};

// Corner UV per vertex, matching the winding order above (0,0)-(1,0)-(1,1)-(0,1).
static const uint8_t uvCorners[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
static const uint32_t voxelIndices[6] = { 0, 1, 2, 2, 3, 0 };

// Placeholder: block palette index, NOT a real texture atlas layer yet.
// Fragment shader will switch on this to pick a solid color.
static uint16_t GetPaletteIndex(BlockType type) {
    return static_cast<uint16_t>(type);
}

Mesh ChunkMesher::MeshChunk(const Chunk& chunk) {
    std::vector<PackedVertex> vertices;
    std::vector<uint32_t> indices;

    for (int y = 0; y < CHUNK_SIZE_Y; y++) {
        for (int z = 0; z < CHUNK_SIZE_Z; z++) {
            for (int x = 0; x < CHUNK_SIZE_X; x++) {
                Block block = chunk.getBlock(x, y, z);
                if (!block.isOpaque()) continue;

                uint16_t palette = GetPaletteIndex(block.type);

                struct Dir { int x, y, z, face; };
                Dir dirs[6] = {
                    {0, 1, 0, 0}, {0, -1, 0, 1},
                    {0, 0, -1, 2}, {0, 0, 1, 3},
                    {1, 0, 0, 4}, {-1, 0, 0, 5}
                };

                for (const auto& dir : dirs) {
                    Block neighbor = chunk.getBlock(x + dir.x, y + dir.y, z + dir.z);
                    if (neighbor.isOpaque()) continue;

                    // Same faux-lighting scheme as before, now packed into skyLight (0-15).
                    float light = 12.0f; // TODO: Don't hardcode this.
                    if (dir.face == 1) light = 0.5f;
                    else if (dir.face == 2 || dir.face == 3) light = 0.8f;
                    else if (dir.face == 4 || dir.face == 5) light = 0.6f;
                    uint8_t skyLight = static_cast<uint8_t>(light * 15.0f);

                    uint32_t baseIndex = static_cast<uint32_t>(vertices.size());

                    for (int i = 0; i < 4; i++) {
                        // Local position: 0-15 range, NOT world-space — chunk offset
                        // is applied via the push-constant model matrix instead.
                        glm::vec3 localPos = glm::vec3(x, y, z) + voxelVertices[dir.face][i];

                        vertices.push_back(PackedVertex::Pack(
                            glm::uvec3(localPos), // truncation is fine, corners are integral
                            static_cast<uint8_t>(dir.face), // normalIndex, 0-5 fits in 3 bits
                            0,                     // AO — not computed yet
                            uvCorners[i][0], uvCorners[i][1],
                            palette,
                            0,                     // blockLight — not tracked yet
                            skyLight
                        ));
                    }

                    for (int i = 0; i < 6; i++) {
                        indices.push_back(baseIndex + voxelIndices[i]);
                    }
                }
            }
        }
    }

    Mesh mesh{};
    mesh.vertexCount = static_cast<uint32_t>(vertices.size());
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.modelMatrix = glm::translate(glm::mat4(1.0f),
        glm::vec3(chunk.getX() * CHUNK_SIZE_X, 0.0f, chunk.getZ() * CHUNK_SIZE_Z));

    if (mesh.vertexCount > 0) {
        EnsureSlabsInitialized();

        VkDeviceSize vertexBufferSize = sizeof(PackedVertex) * vertices.size();
        VkDeviceSize vertexOffset = 0;
        // Align to the full vertex stride (8 bytes), not alignof(PackedVertex)
        // (which is only 4, the alignment of its largest uint32_t member). Using
        // the smaller alignment let vertex sub-buffers start at offsets that were
        // a multiple of 4 but not 8, shifting every subsequent vertex fetch by
        // 4 bytes and corrupting the word0/word1 pairing read by the GPU.
        if (vertexSlab.Allocate(vertices.data(), vertexBufferSize, sizeof(PackedVertex), vertexOffset)) {
            mesh.vertexBuffer = vertexSlab.GetBuffer();
            mesh.vertexOffset = vertexOffset;
        } else {
            mesh.vertexCount = 0;
        }

        VkDeviceSize indexBufferSize = sizeof(uint32_t) * indices.size();
        VkDeviceSize indexOffset = 0;
        if (indexSlab.Allocate(indices.data(), indexBufferSize, sizeof(uint32_t), indexOffset)) {
            mesh.indexBuffer = indexSlab.GetBuffer();
            mesh.indexOffset = indexOffset;
        } else {
            mesh.indexCount = 0;
        }
    }

    return mesh;
}

} // namespace Volcano
