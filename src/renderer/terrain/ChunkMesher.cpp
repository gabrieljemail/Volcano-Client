#include "ChunkMesher.hpp"
#include "models/PackedVertex.hpp"
#include "models/BlockRegistry.hpp"
#include "models/ChunkSection.hpp"
#include "../models/SlabBuffer.hpp"
#include "../VulkanInit.hpp"
#include "../BiomeColors.hpp"
#include "../../Logger.hpp"
#include "../../Config.hpp"
#include <glm/ext/matrix_transform.hpp>
#include <algorithm>
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

// Scales this mesher's base slab size by how many chunk columns
// Graphics.RenderDistance actually loads — (2*RD+1)^2, the same square-view
// accounting vanilla's own simulation distance uses — rather than a single
// fixed budget that's only right for whatever distance it happened to be
// tuned at. Every factor lives in settings.json (Graphics.SlabSize.*) rather
// than being hardcoded, since the right numbers depend entirely on the
// machine: this is a single VkDeviceMemory allocation out of the
// host-visible heap SlabBuffer maps persistently (CreateBuffer's isUMA
// flag), which on an integrated/UMA GPU is real system RAM, and on a
// discrete GPU without resizable BAR is often capped in the low hundreds of
// MB regardless of total VRAM — scaling too aggressively risks that
// allocation failing outright (SlabBuffer::Init now throws a readable error
// instead of crashing on that, but a smaller increase that stays reliable
// beats a bigger one that doesn't). If it's still failing, lower
// baseSizeMBKey and/or maxScalePercent in settings.json.
//
// baseSizeMBKey: the base size in MB (floor — never allocates less than
// this regardless of render distance). ReferenceRenderDistance: the render
// distance that base size is considered "right" for; scale is 1.0 there.
// MaxScalePercent: hard cap on how far render distance can scale the base
// size up (percent, not a fraction — ConfigValue has no float type, same
// convention as InteractionManagerConfig's alpha settings).
static uint64_t SlabSizeForRenderDistance(const std::string& baseSizeMBKey, uint32_t defaultBaseSizeMB) {
    Config* config = Config::Active();
    uint64_t baseSize = (config
        ? std::get<uint32_t>(config->Get(baseSizeMBKey, uint32_t{defaultBaseSizeMB}))
        : defaultBaseSizeMB) * 1024ull * 1024ull;
    uint32_t renderDistance = config
        ? std::get<uint32_t>(config->Get("Graphics.RenderDistance", uint32_t{8}))
        : 8;
    uint32_t referenceRenderDistance = config
        ? std::get<uint32_t>(config->Get("Graphics.SlabSize.ReferenceRenderDistance", uint32_t{4}))
        : 4;
    uint32_t maxScalePercent = config
        ? std::get<uint32_t>(config->Get("Graphics.SlabSize.MaxScalePercent", uint32_t{100}))
        : 100;

    double columns = (2.0 * renderDistance + 1.0) * (2.0 * renderDistance + 1.0);
    double referenceColumns = (2.0 * referenceRenderDistance + 1.0) * (2.0 * referenceRenderDistance + 1.0);
    double scale = std::min(columns / referenceColumns, static_cast<double>(maxScalePercent) / 100.0);
    double scaled = static_cast<double>(baseSize) * scale;
    return std::max<uint64_t>(baseSize, static_cast<uint64_t>(scaled));
}

static void EnsureSlabsInitialized() {
    if (slabsInitialized) return;

    uint64_t slabSize = SlabSizeForRenderDistance("Graphics.SlabSize.CubicMB",
        static_cast<uint32_t>(ChunkMesher::DEFAULT_SLAB_SIZE / (1024ull * 1024ull)));
    slabSize = SlabBuffer::AffordableSize(slabSize, 0.7, 2); // 2 slabs, 70% of the slab budget
    vertexSlab.Init(slabSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    indexSlab.Init(slabSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    slabsInitialized = true;
}

void ChunkMesher::Shutdown() {
    if (!slabsInitialized) return;

    vertexSlab.Destroy();
    indexSlab.Destroy();
    slabsInitialized = false;
}

bool ChunkMesher::ReleaseMesh(const Mesh& mesh) {
    if (!slabsInitialized) return false;
    bool owned = false;
    if (mesh.vertexBytes > 0 && mesh.vertexBuffer == vertexSlab.GetBuffer()) {
        vertexSlab.Free(mesh.vertexOffset, mesh.vertexBytes);
        owned = true;
    }
    if (mesh.indexBytes > 0 && mesh.indexBuffer == indexSlab.GetBuffer()) {
        indexSlab.Free(mesh.indexOffset, mesh.indexBytes);
        owned = true;
    }
    return owned;
}

SlabStats ChunkMesher::GetSlabStats() {
    return SlabStats{
        vertexSlab.GetUsed() + indexSlab.GetUsed(),
        vertexSlab.GetCapacity() + indexSlab.GetCapacity()
    };
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
// the solid block contributing a face. Returns both channels from one lookup
// — see LightSample's own comment.
static LightSample GetLightAcrossChunks(const Chunk& chunk, const World& world, int lx, int ly, int lz) {
    if (lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z) {
        return chunk.getLightSample(lx, ly, lz);
    }

    int worldX = chunk.getX() * CHUNK_SIZE_X + lx;
    int worldZ = chunk.getZ() * CHUNK_SIZE_Z + lz;
    return world.GetLightSample(worldX, ly + WORLD_MIN_Y, worldZ);
}

// One cell of the 2D mask swept across a chunk during greedy meshing: which
// block type is exposed at this cell, and which side of the sweep plane it
// sits on (+1 = solid block is on the negative side of the boundary and its
// face points in the positive axis direction, -1 = the opposite). A `normal`
// of 0 means no face is exposed here (both sides agree — solid/solid or
// air/air) and the cell is skipped. `skyLight`/`blockLight` are the real
// per-block light at the solid block contributing this face, kept apart
// (see LightSample's own comment) — both are part of the mask comparison so
// the greedy sweep never merges two faces with a different light level on
// either channel into one quad (light isn't interpolated across a merged
// quad, so quads simply stop growing at a light boundary instead).
struct MaskCell {
    uint16_t type = 0;
    int8_t normal = 0;
    uint8_t skyLight = 0;
    uint8_t blockLight = 0;

    bool IsEmpty() const { return normal == 0; }
    bool operator==(const MaskCell& other) const {
        return type == other.type && normal == other.normal
            && skyLight == other.skyLight && blockLight == other.blockLight;
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
//
// [yMin, yMax) restricts the sweep to one 16-tall section (see
// MeshChunkSection) instead of the whole 384-tall column — X/Z are never
// restricted (a section is only ever sliced vertically). For axis 1 (Y is
// the sweep-plane axis itself) this simply narrows the plane-sweep range;
// for axis 0/2 (Y is one of the mask's u/v dimensions instead) the mask's
// extent and the sample loop along that dimension both shrink to
// [yMin, yMax) rather than the full chunk height.
static void GreedyMeshAxis(const Chunk& chunk, const World& world, int axis, int yMin, int yMax,
        const TextureManager& textureManager,
        std::vector<PackedVertex>& vertices, std::vector<uint32_t>& indices) {
    const int dims[3] = { CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z };
    int rangeStart[3] = { 0, yMin, 0 };
    int rangeEnd[3] = { dims[0], yMax, dims[2] };

    // `u`/`v` name the other two axes the mask sweeps across, and directly
    // become each quad's texture U/V direction.
    int u, v;
    if (axis == 0) { u = 2; v = 1; }
    else { u = (axis + 1) % 3; v = (axis + 2) % 3; }

    // Mask-local extent along u/v — full chunk width for a horizontal axis,
    // narrowed to the section height for Y (see rangeStart/rangeEnd above).
    int maskW = rangeEnd[u] - rangeStart[u];
    int maskH = rangeEnd[v] - rangeStart[v];
    std::vector<MaskCell> mask(static_cast<size_t>(maskW) * static_cast<size_t>(maskH));

    int x[3] = {0, 0, 0};
    int q[3] = {0, 0, 0};
    q[axis] = 1;

    for (x[axis] = rangeStart[axis] - 1; x[axis] < rangeEnd[axis]; ) {
        int n = 0;
        for (x[v] = rangeStart[v]; x[v] < rangeEnd[v]; x[v]++) {
            for (x[u] = rangeStart[u]; x[u] < rangeEnd[u]; x[u]++, n++) {
                Block a = GetBlockAcrossChunks(chunk, world, x[0], x[1], x[2]);
                Block b = GetBlockAcrossChunks(chunk, world, x[0] + q[0], x[1] + q[1], x[2] + q[2]);
                bool aOpaque = a.isOpaque();
                bool bOpaque = b.isOpaque();

                if (aOpaque == bOpaque) {
                    mask[n] = {};
                } else if (aOpaque) {
                    LightSample light = GetLightAcrossChunks(chunk, world, x[0] + q[0], x[1] + q[1], x[2] + q[2]);
                    mask[n] = { a.visualId, 1, light.sky, light.block };
                } else {
                    LightSample light = GetLightAcrossChunks(chunk, world, x[0], x[1], x[2]);
                    mask[n] = { b.visualId, -1, light.sky, light.block };
                }
            }
        }

        x[axis]++; // `x[axis]` is now the coordinate of the plane the mask describes.

        n = 0;
        for (int j = 0; j < maskH; j++) {
            for (int i = 0; i < maskW;) {
                const MaskCell cell = mask[n];
                if (cell.IsEmpty()) { i++; n++; continue; }

                int w = 1;
                while (i + w < maskW && mask[n + w] == cell) w++;

                int h = 1;
                bool done = false;
                while (j + h < maskH) {
                    for (int k = 0; k < w; k++) {
                        if (mask[n + k + h * maskW] != cell) { done = true; break; }
                    }
                    if (done) break;
                    h++;
                }

                x[u] = rangeStart[u] + i;
                x[v] = rangeStart[v] + j;

                int faceIndex = FaceIndexFor(axis, cell.normal);
                FaceTexture faceTexture = GetFaceTexture(cell.type, faceIndex, textureManager);

                static const int positiveCorners[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
                static const int negativeCorners[4][2] = { {0,0}, {0,1}, {1,1}, {1,0} };
                const bool handednessFlipped = (axis == 0);
                const auto& corners = ((cell.normal > 0) != handednessFlipped) ? positiveCorners : negativeCorners;

                uint32_t baseIndex = static_cast<uint32_t>(vertices.size());
                for (int c = 0; c < 4; c++) {
                    int uOff = corners[c][0] * w;
                    int vOff = corners[c][1] * h;

                    // Local position: 0-15 range on every axis, not world-space.
                    int localPos[3] = { x[0], x[1], x[2] };
                    localPos[u] = x[u] + uOff;
                    localPos[v] = x[v] + vOff;
                    localPos[1] -= yMin;

                    int texV = (v == 1) ? (h - vOff) : vOff;

                    // Compute AO for this vertex.
                    uint8_t ao = ChunkMesher::ComputeVertexAO(chunk, world, u, v, axis,
                            corners[c][0], corners[c][1],
                            x[u], x[v], x[axis] + (cell.normal > 0 ? 0 : -1)
                    );

                    vertices.push_back(PackedVertex::Pack(
                        glm::uvec3(localPos[0], localPos[1], localPos[2]),
                        static_cast<uint8_t>(faceIndex), // normalIndex, 0-5 fits in 3 bits.
                        ao,
                        static_cast<uint8_t>(uOff), static_cast<uint8_t>(texV),
                        faceTexture.layer,
                        cell.blockLight,
                        cell.skyLight,
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
                        mask[n + ww + hh * maskW] = {};
                    }
                }

                i += w;
                n += w;
            }
        }
    }
}

// A helper function to compute vertex light when meshing chunks for smooth lighting.

// A helper function to compute vertex AO when meshing chunks for smooth lighting.
uint8_t ChunkMesher::ComputeVertexAO(const Chunk& chunk, const World& world,
    int u, int v, int n,                // Face plane xes.
    int cornerU, int cornerV,           // Corner (0-3).
    int baseU, int baseV, int baseN     // Face position.
) {
    // Map directon of each vertex from the origin.
    int offsetU = cornerU ? 1 : -1;
    int offsetV = cornerV ? 1 : -1;

    // Check three neighbors in the same plane.
    int pos[3];
    pos[u] = baseU;
    pos[v] = baseV;
    pos[n] = baseN;

    int occlusions = 0;

    // Check U neighbor.
    int checkU[3] = {pos[0], pos[1], pos[2]};
    checkU[u] = baseU + offsetU;
    Block bU = GetBlockAcrossChunks(chunk, world, checkU[0], checkU[1], checkU[2]);
    if (bU.isOpaque()) occlusions++;

    // Check V neighbor.
    int checkV[3] = {pos[0], pos[1], pos[2]};
    checkV[v] = baseV + offsetV;
    Block bV = GetBlockAcrossChunks(chunk, world, checkV[0], checkV[1], checkV[2]);
    if (bV.isOpaque()) occlusions++;

    // Check diagonally, but only if both axial neighbors exist.
    if (occlusions == 2)
    {
        int checkD[3] = {pos[0], pos[1], pos[2]};
        checkD[u] = baseU + offsetU;
        checkD[v] = baseV + offsetV;
        Block bD = GetBlockAcrossChunks(chunk, world, checkD[0], checkD[1], checkD[2]);
        if (bD.isOpaque()) occlusions++;
    }

    return static_cast<uint8_t>(occlusions);
}

Mesh ChunkMesher::MeshChunkSection(const Chunk& chunk, const World& world, const TextureManager& textureManager, int sectionY) {
    std::vector<PackedVertex> vertices;
    std::vector<uint32_t> indices;

    int yMin = sectionY * SECTION_SIZE_Y;
    int yMax = yMin + SECTION_SIZE_Y;
    for (int axis = 0; axis < 3; axis++) {
        GreedyMeshAxis(chunk, world, axis, yMin, yMax, textureManager, vertices, indices);
    }

    Mesh mesh{};
    mesh.vertexCount = static_cast<uint32_t>(vertices.size());
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.modelMatrix = glm::translate(glm::mat4(1.0f),
        glm::vec3(chunk.getX() * CHUNK_SIZE_X, WORLD_MIN_Y + yMin, chunk.getZ() * CHUNK_SIZE_Z));

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
            mesh.vertexBytes = vertexBufferSize;
        } else {
            // Slab genuinely full (replaced meshes are handed back via
            // ReleaseMesh, so this means live geometry outgrew the slab —
            // raise Graphics.SlabSize.CubicMB). Logged rather than silently
            // dropped so a section that stops updating is diagnosable.
            Log::Error("[MESH] cube vertex slab full (chunk " + std::to_string(chunk.getX()) + ","
                + std::to_string(chunk.getZ()) + ", section " + std::to_string(sectionY) + ") — section not updated.");
            mesh.vertexCount = 0;
        }

        VkDeviceSize indexBufferSize = sizeof(uint32_t) * indices.size();
        VkDeviceSize indexOffset = 0;
        if (indexSlab.Allocate(indices.data(), indexBufferSize, sizeof(uint32_t), indexOffset)) {
            mesh.indexBuffer = indexSlab.GetBuffer();
            mesh.indexOffset = indexOffset;
            mesh.indexBytes = indexBufferSize;
        } else {
            Log::Error("[MESH] cube index slab full (chunk " + std::to_string(chunk.getX()) + ","
                + std::to_string(chunk.getZ()) + ", section " + std::to_string(sectionY) + ") — section not updated.");
            mesh.indexCount = 0;
        }

        // Half a mesh is useless — give back whichever half did fit.
        if (mesh.vertexCount == 0 || mesh.indexCount == 0) {
            ReleaseMesh(mesh);
            mesh = Mesh{};
        }
    }

    return mesh;
}

} // namespace Volcano
