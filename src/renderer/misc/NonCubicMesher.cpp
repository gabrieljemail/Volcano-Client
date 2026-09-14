#include "NonCubicMesher.hpp"
#include "MiscVertex.hpp"
#include "../terrain/models/BlockRegistry.hpp"
#include "../models/SlabBuffer.hpp"
#include "../VulkanInit.hpp"
#include "../BiomeColors.hpp"
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <array>
#include <cmath>

namespace Volcano {

// Same "one slab pair, bump-allocated per chunk mesh" scheme ChunkMesher
// uses — see its own comment for why (streamed-in chunks, avoiding a
// VkBuffer/VmaAllocation per chunk). Kept entirely separate from
// ChunkMesher's slabs since this mesh is bound by a different pipeline
// (nonCubicPipeline) with a different vertex format (MiscVertex).
static SlabBuffer vertexSlab;
static SlabBuffer indexSlab;
static bool slabsInitialized = false;

static void EnsureSlabsInitialized() {
    if (slabsInitialized) return;

    vertexSlab.Init(NonCubicMesher::DEFAULT_SLAB_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    indexSlab.Init(NonCubicMesher::DEFAULT_SLAB_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    slabsInitialized = true;
}

void NonCubicMesher::Shutdown() {
    if (!slabsInitialized) return;

    vertexSlab.Destroy();
    indexSlab.Destroy();
    slabsInitialized = false;
}

// Neighbor offset for each of ChunkMesher's 6 face indices — used both to
// cull a transparent-cube face against an identical neighbor (see
// MeshTransparentCube) and to sample the real light a face is exposed to.
static constexpr int FACE_DX[6] = { 0, 0, 0, 0, 1, -1 };
static constexpr int FACE_DY[6] = { 1, -1, 0, 0, 0, 0 };
static constexpr int FACE_DZ[6] = { 0, 0, -1, 1, 0, 0 };

// Reads a block that may be outside this chunk's own 0..CHUNK_SIZE_X-1 /
// 0..CHUNK_SIZE_Z-1 horizontal range by falling back to World for anything
// that crosses into a neighboring chunk — same reasoning and shape as
// ChunkMesher's own GetBlockAcrossChunks. Without this, a transparent block
// sitting against the same visual across a chunk seam (e.g. a glass wall
// spanning two chunks) had its shared face drawn from both sides instead of
// culled, since Chunk::getBlock alone has no way to see past its own
// bounds and just reports Air there.
static Block GetBlockAcrossChunks(const Chunk& chunk, const World& world, int lx, int ly, int lz) {
    if (lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z) {
        return chunk.getBlock(lx, ly, lz);
    }

    int worldX = chunk.getX() * CHUNK_SIZE_X + lx;
    int worldZ = chunk.getZ() * CHUNK_SIZE_Z + lz;
    return world.GetBlock(worldX, ly + WORLD_MIN_Y, worldZ);
}

// Same cross-chunk fallback, for the light sample at the block a face is
// exposed to (see ChunkMesher's own GetLightAcrossChunks).
static uint8_t GetLightAcrossChunks(const Chunk& chunk, const World& world, int lx, int ly, int lz) {
    if (lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z) {
        return chunk.getLight(lx, ly, lz);
    }

    int worldX = chunk.getX() * CHUNK_SIZE_X + lx;
    int worldZ = chunk.getZ() * CHUNK_SIZE_Z + lz;
    return world.GetLight(worldX, ly + WORLD_MIN_Y, worldZ);
}

// Four corners of one face of an axis-aligned box, in some consistent
// (not necessarily outward-CCW) winding — the non-cubic pipeline disables
// backface culling entirely (see VulkanInit's nonCubicPipeline), so unlike
// ChunkMesher/EntityRenderer there's no winding convention to satisfy here.
static std::array<glm::vec3, 4> FaceCorners(const glm::vec3& min, const glm::vec3& max, int faceIndex) {
    switch (faceIndex) {
        case 0: return { glm::vec3(min.x, max.y, min.z), glm::vec3(max.x, max.y, min.z),
                          glm::vec3(max.x, max.y, max.z), glm::vec3(min.x, max.y, max.z) }; // Up
        case 1: return { glm::vec3(min.x, min.y, min.z), glm::vec3(min.x, min.y, max.z),
                          glm::vec3(max.x, min.y, max.z), glm::vec3(max.x, min.y, min.z) }; // Down
        case 2: return { glm::vec3(max.x, min.y, min.z), glm::vec3(min.x, min.y, min.z),
                          glm::vec3(min.x, max.y, min.z), glm::vec3(max.x, max.y, min.z) }; // North (-Z)
        case 3: return { glm::vec3(min.x, min.y, max.z), glm::vec3(max.x, min.y, max.z),
                          glm::vec3(max.x, max.y, max.z), glm::vec3(min.x, max.y, max.z) }; // South (+Z)
        case 4: return { glm::vec3(max.x, min.y, max.z), glm::vec3(max.x, min.y, min.z),
                          glm::vec3(max.x, max.y, min.z), glm::vec3(max.x, max.y, max.z) }; // East (+X)
        default: return { glm::vec3(min.x, min.y, min.z), glm::vec3(min.x, min.y, max.z),
                          glm::vec3(min.x, max.y, max.z), glm::vec3(min.x, max.y, min.z) }; // West (-X)
    }
}

// uvRect is (u1,v1,u2,v2) in 0..16 model-space texture units (matching the
// model JSON's own "uv" — see NonCubeElement::faceUVs), normalized here to
// 0..1. Corner order matches FaceCorners' — (u1,v1) at corner 0 through
// (u1,v2) at corner 3 — the same parametrization a full 0,0,16,16 rect
// degenerates to, so this is a strict generalization of the old hardcoded
// full-texture UVs, not a behavior change for anything that already used
// the whole texture (full cubes, cross planes).
static void EmitQuad(std::vector<MiscVertex>& vertices, std::vector<uint32_t>& indices,
        const std::array<glm::vec3, 4>& corners, const glm::vec4& uvRect,
        uint16_t textureLayer, bool biomeTinted, uint8_t light) {
    glm::vec2 uvMin = glm::vec2(uvRect.x, uvRect.y) / 16.0f;
    glm::vec2 uvMax = glm::vec2(uvRect.z, uvRect.w) / 16.0f;
    const std::array<glm::vec2, 4> uvs = {
        glm::vec2(uvMin.x, uvMin.y), glm::vec2(uvMax.x, uvMin.y),
        glm::vec2(uvMax.x, uvMax.y), glm::vec2(uvMin.x, uvMax.y)
    };

    uint32_t base = static_cast<uint32_t>(vertices.size());
    uint32_t packed = MiscVertex::Pack(textureLayer, light, biomeTinted);

    for (int i = 0; i < 4; i++) {
        vertices.push_back(MiscVertex{ corners[static_cast<size_t>(i)], uvs[static_cast<size_t>(i)], packed });
    }

    indices.insert(indices.end(), { base, base + 1, base + 2, base + 2, base + 3, base });
}

// Emits every present face of one model element (a box in 0..16 model-space
// units) unconditionally — partial shapes (slabs, carpets, ...) are rare
// enough, and varied enough in footprint, that neighbor-based face culling
// isn't worth the complexity ChunkMesher's greedy sweep uses for full cubes.
static void MeshElementFaces(const BlockRegistry::NonCubeElement& element, const Chunk& chunk, const World& world,
        int bx, int by, int bz, const glm::vec3& blockOrigin, const TextureManager& textureManager,
        std::vector<MiscVertex>& vertices, std::vector<uint32_t>& indices) {
    glm::vec3 boxMin = blockOrigin + element.from / 16.0f;
    glm::vec3 boxMax = blockOrigin + element.to / 16.0f;

    for (int f = 0; f < 6; f++) {
        const std::string& textureName = element.faceTextures[static_cast<size_t>(f)];
        if (textureName.empty()) continue;

        uint16_t layer = textureManager.GetLayerIndex(textureName);
        bool tinted = IsBiomeTinted(textureName);
        uint8_t light = GetLightAcrossChunks(chunk, world, bx + FACE_DX[f], by + FACE_DY[f], bz + FACE_DZ[f]);
        EmitQuad(vertices, indices, FaceCorners(boxMin, boxMax, f), element.faceUVs[static_cast<size_t>(f)],
            layer, tinted, light);
    }
}

// Transparent full cubes (glass, slime, ice, ...) skip a face when the
// immediate neighbor shares the exact same non-cube visual — same-type
// glass panes don't show their shared internal face. Any other neighbor
// (air, a different transparent visual, an opaque block) still draws the
// face; the opaque case relies on depth testing rather than culling to look
// correct, same trade-off ChunkMesher's own opaque/opaque cull doesn't need
// to make here since this pass never culls against opaque neighbors.
static void MeshTransparentCube(const BlockRegistry::NonCubeElement& element, uint16_t visualId,
        const Chunk& chunk, const World& world, int bx, int by, int bz, const glm::vec3& blockOrigin,
        const TextureManager& textureManager, std::vector<MiscVertex>& vertices, std::vector<uint32_t>& indices) {
    glm::vec3 boxMin = blockOrigin + element.from / 16.0f;
    glm::vec3 boxMax = blockOrigin + element.to / 16.0f;

    for (int f = 0; f < 6; f++) {
        const std::string& textureName = element.faceTextures[static_cast<size_t>(f)];
        if (textureName.empty()) continue;

        Block neighbor = GetBlockAcrossChunks(chunk, world, bx + FACE_DX[f], by + FACE_DY[f], bz + FACE_DZ[f]);
        if (neighbor.nonCubeVisualId == visualId) continue;

        uint16_t layer = textureManager.GetLayerIndex(textureName);
        bool tinted = IsBiomeTinted(textureName);
        uint8_t light = GetLightAcrossChunks(chunk, world, bx + FACE_DX[f], by + FACE_DY[f], bz + FACE_DZ[f]);
        EmitQuad(vertices, indices, FaceCorners(boxMin, boxMax, f), element.faceUVs[static_cast<size_t>(f)],
            layer, tinted, light);
    }
}

// N billboard planes through the block's center, evenly spaced across 180°
// (each plane is visible from both sides — the non-cubic pipeline disables
// backface culling — so 180° of spacing gives N crossing planes, matching
// vanilla's 2-plane X-shape at CROSS_BILLBOARD_COUNT's default of 2: 45° and
// 135°, the same pair of diagonals vanilla's block/cross model uses).
static void MeshCross(const BlockRegistry::NonCubeVisual& visual, const Chunk& chunk,
        int bx, int by, int bz, const glm::vec3& blockOrigin, const TextureManager& textureManager,
        std::vector<MiscVertex>& vertices, std::vector<uint32_t>& indices) {
    uint16_t layer = textureManager.GetLayerIndex(visual.crossTexture);
    bool tinted = IsBiomeTinted(visual.crossTexture);
    // Flat — a billboard has no single well-defined face normal to sample a neighbor by; use the block's own light.
    uint8_t light = chunk.getLight(bx, by, bz);

    glm::vec3 center = blockOrigin + glm::vec3(0.5f, 0.0f, 0.5f);
    int planeCount = CROSS_BILLBOARD_COUNT > 0 ? CROSS_BILLBOARD_COUNT : 1;

    for (int i = 0; i < planeCount; i++) {
        float angle = glm::radians(45.0f + 180.0f * static_cast<float>(i) / static_cast<float>(planeCount));
        glm::vec3 half = glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * 0.5f;

        glm::vec3 p0 = center - half;
        glm::vec3 p1 = center + half;

        std::array<glm::vec3, 4> corners = {
            glm::vec3(p0.x, 0.0f, p0.z),
            glm::vec3(p1.x, 0.0f, p1.z),
            glm::vec3(p1.x, 1.0f, p1.z),
            glm::vec3(p0.x, 1.0f, p0.z),
        };
        EmitQuad(vertices, indices, corners, glm::vec4(0.0f, 0.0f, 16.0f, 16.0f), layer, tinted, light);
    }
}

Mesh NonCubicMesher::MeshChunk(const Chunk& chunk, const World& world, const TextureManager& textureManager) {
    std::vector<MiscVertex> vertices;
    std::vector<uint32_t> indices;

    for (int y = 0; y < CHUNK_SIZE_Y; y++) {
        for (int z = 0; z < CHUNK_SIZE_Z; z++) {
            for (int x = 0; x < CHUNK_SIZE_X; x++) {
                Block block = chunk.getBlock(x, y, z);
                if (block.nonCubeVisualId == 0) continue;

                const BlockRegistry::NonCubeVisual& visual = BlockRegistry::GetNonCubeVisual(block.nonCubeVisualId);
                glm::vec3 blockOrigin(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));

                if (visual.shape == BlockRegistry::NonCubeShape::Cross) {
                    MeshCross(visual, chunk, x, y, z, blockOrigin, textureManager, vertices, indices);
                    continue;
                }

                for (const BlockRegistry::NonCubeElement& element : visual.elements) {
                    if (visual.shape == BlockRegistry::NonCubeShape::TransparentCube) {
                        MeshTransparentCube(element, block.nonCubeVisualId, chunk, world, x, y, z, blockOrigin,
                            textureManager, vertices, indices);
                    } else {
                        MeshElementFaces(element, chunk, world, x, y, z, blockOrigin, textureManager, vertices, indices);
                    }
                }
            }
        }
    }

    Mesh mesh{};
    mesh.vertexCount = static_cast<uint32_t>(vertices.size());
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.modelMatrix = glm::translate(glm::mat4(1.0f),
        glm::vec3(chunk.getX() * CHUNK_SIZE_X, WORLD_MIN_Y, chunk.getZ() * CHUNK_SIZE_Z));

    if (mesh.vertexCount > 0) {
        EnsureSlabsInitialized();

        VkDeviceSize vertexBufferSize = sizeof(MiscVertex) * vertices.size();
        VkDeviceSize vertexOffset = 0;
        if (vertexSlab.Allocate(vertices.data(), vertexBufferSize, sizeof(MiscVertex), vertexOffset)) {
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
