#include "ChunkMesher.hpp"
#include "models/PackedVertex.hpp"
#include "models/BlockRegistry.hpp"
#include "../models/SlabBuffer.hpp"
#include "../VulkanInit.hpp"
#include "../BiomeColors.hpp"
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

// Reads a block that may be outside this chunk's own 0..CHUNK_SIZE_X-1 /
// 0..CHUNK_SIZE_Z-1 horizontal range (the greedy sweep below deliberately
// samples one step past each edge to test exposure there) by falling back
// to World for anything horizontal that crosses into a neighboring chunk.
// Vertical range never needs this — Chunk::getBlock's own bounds check
// already returns Air above/below the world height correctly, no neighbor
// chunk involved — so only lx/lz are tested here. The in-bounds case is a
// plain array index (chunk.getBlock), same cost as before; only the two
// edge planes per horizontal axis pay for a World lookup (chunk-map lookup
// + shared_lock), not every sample.
static Block GetBlockAcrossChunks(const Chunk& chunk, const World& world, int lx, int ly, int lz) {
    if (lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z) {
        return chunk.getBlock(lx, ly, lz);
    }

    int worldX = chunk.getX() * CHUNK_SIZE_X + lx;
    int worldZ = chunk.getZ() * CHUNK_SIZE_Z + lz;
    return world.GetBlock(worldX, ly + WORLD_MIN_Y, worldZ);
}

// Same cross-chunk fallback as GetBlockAcrossChunks, for the light sample at
// the solid block contributing a face.
static uint8_t GetLightAcrossChunks(const Chunk& chunk, const World& world, int lx, int ly, int lz) {
    if (lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z) {
        return chunk.getLight(lx, ly, lz);
    }

    int worldX = chunk.getX() * CHUNK_SIZE_X + lx;
    int worldZ = chunk.getZ() * CHUNK_SIZE_Z + lz;
    return world.GetLight(worldX, ly + WORLD_MIN_Y, worldZ);
}

// One cell of the 2D mask swept across a chunk during greedy meshing: which
// block type is exposed at this cell, and which side of the sweep plane it
// sits on (+1 = solid block is on the negative side of the boundary and its
// face points in the positive axis direction, -1 = the opposite). A `normal`
// of 0 means no face is exposed here (both sides agree — solid/solid or
// air/air) and the cell is skipped. `light` is the real per-block light at
// the solid block contributing this face; it's part of the mask comparison
// so the greedy sweep never merges two faces with different light levels
// into one quad (light isn't interpolated across a merged quad, so quads
// simply stop growing at a light boundary instead).
struct MaskCell {
    uint16_t type = 0;
    int8_t normal = 0;
    uint8_t light = 0;

    bool IsEmpty() const { return normal == 0; }
    bool operator==(const MaskCell& other) const {
        return type == other.type && normal == other.normal && light == other.light;
    }
    bool operator!=(const MaskCell& other) const { return !(*this == other); }
};

// Maps a sweep axis (0=X, 1=Y, 2=Z) and the sign of the exposed face's normal
// to the same face indices GetFaceTexture()/the lighting table below expect.
static int FaceIndexFor(int axis, int normal) {
    switch (axis) {
        case 0: return normal > 0 ? 4 : 5; // East / West
        case 1: return normal > 0 ? 0 : 1; // Up / Down
        default: return normal > 0 ? 3 : 2; // South / North
    }
}

struct FaceTexture {
    uint16_t layer;
    bool biomeTinted;
};

// Maps a block's visual id (and, for blocks whose faces differ, which face
// is being meshed) to the resource-pack texture name via BlockRegistry
// (built from minecraft-data + the vanilla blockstate/model JSON at
// startup), then resolves that name to its array layer via the
// TextureManager. The array layer for a texture is assigned by load order
// (see TextureManager::LoadResourcePack), which has no relation to the
// visual id, so the two must never be conflated (using `visualId` directly
// as the layer index).
static FaceTexture GetFaceTexture(uint16_t visualId, int face, const TextureManager& textureManager) {
    const std::string& name = BlockRegistry::GetFaceTextureName(visualId, face);
    return { textureManager.GetLayerIndex(name), IsBiomeTinted(name) };
}

// Sweeps every boundary plane perpendicular to `axis` (0=X, 1=Y, 2=Z), and for
// each plane greedily merges the exposed faces on it into maximal rectangles
// (standard "Meshing in a Minecraft Game" algorithm) instead of emitting one
// quad per block face. `u`/`v` name the other two axes, in the order the mask
// is swept in.
static void GreedyMeshAxis(const Chunk& chunk, const World& world, int axis, const TextureManager& textureManager,
        std::vector<PackedVertex>& vertices, std::vector<uint32_t>& indices) {
    const int dims[3] = { CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z };

    // u/v name the other two axes the mask sweeps across, and directly
    // become each quad's texture U/V direction (see the UV-packing comment
    // below) — so for the two "side" axes (X=0, Z=2), v must be Y (world-
    // vertical) or the texture ends up rotated 90° on that wall. Z (axis 2)
    // already got this right via the plain (axis+1)%3/(axis+2)%3 formula
    // (u=X horizontal, v=Y vertical); X (axis 0) didn't (u=Y, v=Z — texture
    // U driven by height instead of depth), which is what put a sideways/
    // vertical grain on every X-facing wall. Y (axis 1, floor/ceiling) is
    // unaffected either way since both remaining axes are horizontal.
    int u, v;
    if (axis == 0) { u = 2; v = 1; }
    else { u = (axis + 1) % 3; v = (axis + 2) % 3; }

    std::vector<MaskCell> mask(dims[u] * dims[v]);

    int x[3] = {0, 0, 0};
    int q[3] = {0, 0, 0};
    q[axis] = 1;

    // Planes run from -1 (chunk's negative boundary, block A is out-of-bounds
    // Air) through dims[axis]-1 (chunk's positive boundary, block B is Air).
    // Chunk::getBlock() already clamps out-of-range coordinates to Air, so no
    // special-casing is needed at the edges.
    //
    // No auto-increment here: the loop body already advances x[axis] by 1
    // itself (the manual `x[axis]++` below, needed to turn the "a" sample
    // coordinate into the plane coordinate quads are built at). Auto-
    // incrementing here TOO meant x[axis] advanced by 2 every iteration,
    // silently skipping every other boundary along this axis (testing
    // -1|0, 1|2, 3|4, ... and never 0|1, 2|3, 4|5, ...) — roughly half of
    // every chunk's internal block boundaries were never checked for
    // exposure at all, on every axis. That's the actual "solid block with
    // a missing face" bug, unrelated to winding/culling/merging.
    for (x[axis] = -1; x[axis] < dims[axis]; ) {
        int n = 0;
        for (x[v] = 0; x[v] < dims[v]; x[v]++) {
            for (x[u] = 0; x[u] < dims[u]; x[u]++, n++) {
                Block a = GetBlockAcrossChunks(chunk, world, x[0], x[1], x[2]);
                Block b = GetBlockAcrossChunks(chunk, world, x[0] + q[0], x[1] + q[1], x[2] + q[2]);
                bool aOpaque = a.isOpaque();
                bool bOpaque = b.isOpaque();

                if (aOpaque == bOpaque) {
                    mask[n] = {};
                } else if (aOpaque) {
                    uint8_t light = GetLightAcrossChunks(chunk, world, x[0], x[1], x[2]);
                    mask[n] = { a.visualId, 1, light };
                } else {
                    uint8_t light = GetLightAcrossChunks(chunk, world, x[0] + q[0], x[1] + q[1], x[2] + q[2]);
                    mask[n] = { b.visualId, -1, light };
                }
            }
        }

        x[axis]++; // x[axis] is now the coordinate of the plane the mask describes

        n = 0;
        for (int j = 0; j < dims[v]; j++) {
            for (int i = 0; i < dims[u];) {
                const MaskCell cell = mask[n];
                if (cell.IsEmpty()) { i++; n++; continue; }

                int w = 1;
                while (i + w < dims[u] && mask[n + w] == cell) w++;

                int h = 1;
                bool done = false;
                while (j + h < dims[v]) {
                    for (int k = 0; k < w; k++) {
                        if (mask[n + k + h * dims[u]] != cell) { done = true; break; }
                    }
                    if (done) break;
                    h++;
                }

                x[u] = i;
                x[v] = j;

                int faceIndex = FaceIndexFor(axis, cell.normal);
                FaceTexture faceTexture = GetFaceTexture(cell.type, faceIndex, textureManager);

                uint8_t skyLight = cell.light;

                // Quad corners as (u, v) offsets from (x[u], x[v]); the winding
                // order differs by normal sign so the merged quad still faces
                // outward correctly (matches the per-face winding the old
                // per-voxel table used). Which specific winding is "correct"
                // no longer matters for visual correctness — see
                // VulkanInit.cpp's terrain pipeline, which now disables
                // backface culling entirely rather than relying on getting
                // this exactly right for every axis/viewing angle.
                static const int positiveCorners[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
                static const int negativeCorners[4][2] = { {0,0}, {0,1}, {1,1}, {1,0} };
                const auto& corners = cell.normal > 0 ? positiveCorners : negativeCorners;

                uint32_t baseIndex = static_cast<uint32_t>(vertices.size());
                for (int c = 0; c < 4; c++) {
                    int uOff = corners[c][0] * w;
                    int vOff = corners[c][1] * h;

                    // Local position: 0-15/0-63 range, NOT world-space — chunk
                    // offset is applied via the push-constant model matrix instead.
                    int localPos[3] = { x[0], x[1], x[2] };
                    localPos[u] = x[u] + uOff;
                    localPos[v] = x[v] + vOff;

                    // UV is packed as raw block-space tile counts (not normalized
                    // 0-1) so a merged quad tiles its texture `w` x `h` times via
                    // the texture sampler's REPEAT wrap mode, rather than
                    // stretching one texture across the whole rect.
                    vertices.push_back(PackedVertex::Pack(
                        glm::uvec3(localPos[0], localPos[1], localPos[2]),
                        static_cast<uint8_t>(faceIndex), // normalIndex, 0-5 fits in 3 bits
                        0,                     // AO — not computed yet
                        static_cast<uint8_t>(uOff), static_cast<uint8_t>(vOff),
                        faceTexture.layer,
                        0,                     // blockLight — combined into skyLight instead, see Chunk::getLight
                        skyLight,
                        faceTexture.biomeTinted
                    ));
                }

                indices.push_back(baseIndex + 0);
                indices.push_back(baseIndex + 1);
                indices.push_back(baseIndex + 2);
                indices.push_back(baseIndex + 2);
                indices.push_back(baseIndex + 3);
                indices.push_back(baseIndex + 0);

                for (int hh = 0; hh < h; hh++) {
                    for (int ww = 0; ww < w; ww++) {
                        mask[n + ww + hh * dims[u]] = {};
                    }
                }

                i += w;
                n += w;
            }
        }
    }
}

Mesh ChunkMesher::MeshChunk(const Chunk& chunk, const World& world, const TextureManager& textureManager) {
    std::vector<PackedVertex> vertices;
    std::vector<uint32_t> indices;

    for (int axis = 0; axis < 3; axis++) {
        GreedyMeshAxis(chunk, world, axis, textureManager, vertices, indices);
    }

    Mesh mesh{};
    mesh.vertexCount = static_cast<uint32_t>(vertices.size());
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.modelMatrix = glm::translate(glm::mat4(1.0f),
        glm::vec3(chunk.getX() * CHUNK_SIZE_X, WORLD_MIN_Y, chunk.getZ() * CHUNK_SIZE_Z));

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
