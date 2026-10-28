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
// Graphics.RenderDistance actually loads - (2*RD+1)^2, the same square-view
// accounting vanilla's own simulation distance uses
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
    if (mesh.vertexBytes > 0 && vertexSlab.Owns(mesh.vertexBuffer)) {
        vertexSlab.Free(mesh.vertexBuffer, mesh.vertexOffset, mesh.vertexBytes);
        owned = true;
    }
    if (mesh.indexBytes > 0 && indexSlab.Owns(mesh.indexBuffer)) {
        indexSlab.Free(mesh.indexBuffer, mesh.indexOffset, mesh.indexBytes);
        owned = true;
    }
    return owned;
}

SlabStats ChunkMesher::GetSlabStats() {
    return SlabStats{
        vertexSlab.GetUsed() + indexSlab.GetUsed(),
        vertexSlab.GetCapacity() + indexSlab.GetCapacity(),
        vertexSlab.GetLimit() + indexSlab.GetLimit(),
        vertexSlab.GetBlockCount() + indexSlab.GetBlockCount()
    };
}

// Faces: 0=Up, 1=Down, 2=North(-Z), 3=South(+Z), 4=East(+X), 5=West(-X)

// Reads a block that may be outside this chunk's own 0..CHUNK_SIZE_X-1 /
// 0..CHUNK_SIZE_Z-1 horizontal range (the greedy sweep below deliberately
// samples one step past each edge to test exposure there) by falling back
// to World for anything horizontal that crosses into a neighboring chunk.
// Vertical range never needs this
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
// - see LightSample's own comment.
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
// of 0 means no face is exposed here (both sides agree - solid/solid or
// air/air) and the cell is skipped.
//
// The cell's four corners each carry their own AO and light (indexed
// [cornerU][cornerV], i.e. the cell's own u/v corner, not the quad's). They
// are part of the comparison, so the sweep only merges cells whose corners all
// agree: shading is interpolated across a quad, and a merged quad would
// otherwise stretch its outer corners over cells it doesn't describe.
struct MaskCell {
    uint16_t type = 0;
    int8_t normal = 0;
    uint8_t ao[2][2] = {};
    uint8_t skyLight[2][2] = {};
    uint8_t blockLight[2][2] = {};

    bool IsEmpty() const { return normal == 0; }
    bool operator==(const MaskCell& other) const {
        return type == other.type && normal == other.normal
            && std::memcmp(ao, other.ao, sizeof(ao)) == 0
            && std::memcmp(skyLight, other.skyLight, sizeof(skyLight)) == 0
            && std::memcmp(blockLight, other.blockLight, sizeof(blockLight)) == 0;
    }
    bool operator!=(const MaskCell& other) const { return !(*this == other); }
};

// Fills `cell`'s per-corner AO and light for the face whose air-side block is
// at (pu, pv, pn) along the sweep's u/v/axis. Reads the 3x3 neighbourhood in
// the air-side layer once. AO is the usual 0-3 (both sides solid = 3, else
// sides plus the corner block); smooth light averages the open blocks around
// the corner, and falls back to the face's own light when smoothing is off.
static void ComputeCornerShading(const Chunk& chunk, const World& world, int u, int v, int axis,
        int pu, int pv, int pn, bool smoothLighting, MaskCell& cell) {
    bool solid[3][3];
    LightSample light[3][3];
    for (int dv = -1; dv <= 1; dv++) {
        for (int du = -1; du <= 1; du++) {
            int pos[3];
            pos[u] = pu + du;
            pos[v] = pv + dv;
            pos[axis] = pn;
            solid[du + 1][dv + 1] = GetBlockAcrossChunks(chunk, world, pos[0], pos[1], pos[2]).isOpaque();
            light[du + 1][dv + 1] = GetLightAcrossChunks(chunk, world, pos[0], pos[1], pos[2]);
        }
    }

    for (int cu = 0; cu < 2; cu++) {
        for (int cv = 0; cv < 2; cv++) {
            int du = cu ? 1 : -1;
            int dv = cv ? 1 : -1;
            bool sideU = solid[1 + du][1];
            bool sideV = solid[1][1 + dv];
            bool corner = solid[1 + du][1 + dv];
            cell.ao[cu][cv] = (sideU && sideV) ? 3 : static_cast<uint8_t>(sideU + sideV + corner);

            if (!smoothLighting) {
                cell.skyLight[cu][cv] = light[1][1].sky;
                cell.blockLight[cu][cv] = light[1][1].block;
                continue;
            }

            // The face's own block always counts; a side block counts when open; the
            // diagonal counts when open and at least one side is (light can't leak
            // around a closed corner).
            int count = 1;
            int sky = light[1][1].sky;
            int block = light[1][1].block;
            auto add = [&](const LightSample& sample) { sky += sample.sky; block += sample.block; count++; };
            if (!sideU) add(light[1 + du][1]);
            if (!sideV) add(light[1][1 + dv]);
            if (!corner && !(sideU && sideV)) add(light[1 + du][1 + dv]);
            cell.skyLight[cu][cv] = static_cast<uint8_t>((sky + count / 2) / count);
            cell.blockLight[cu][cv] = static_cast<uint8_t>((block + count / 2) / count);
        }
    }
}

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
    uint8_t biomeTint; // BiomeColors.hpp's BiomeTint.
    uint8_t shininess; // 0 matte .. 7 mirror-like, for terrain.frag's specular.
};

// How shiny a block looks, guessed from its texture name (the client has no
// per-block material table, same trick as BlockSounds).
static uint8_t ShininessFor(const std::string& texture) {
    auto has = [&](const char* part) { return texture.find(part) != std::string::npos; };
    if (has("glass") || has("ice")) return 7;
    if (has("iron_block") || has("gold_block") || has("diamond_block") || has("emerald_block")
        || has("netherite_block") || has("lapis_block") || has("amethyst_block")) return 6;
    if (has("obsidian")) return 5;
    if (has("copper_block") || has("cut_copper") || has("polished") || has("quartz") || has("smooth")) return 3;
    if (has("snow")) return 2;
    return 0;
}

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
    return { textureManager.GetLayerIndex(name), GetTerrainTint(name), ShininessFor(name) };
}

// Sweeps every boundary plane perpendicular to `axis` (0=X, 1=Y, 2=Z), and for
// each plane greedily merges the exposed faces on it into maximal rectangles
// (standard "Meshing in a Minecraft Game" algorithm) instead of emitting one
// quad per block face. `u`/`v` name the other two axes, in the order the mask
// is swept in.
//
// [yMin, yMax) restricts the sweep to one 16-tall section (see
// MeshChunkSection) instead of the whole 384-tall column - X/Z are never
// restricted (a section is only ever sliced vertically). For axis 1 (Y is
// the sweep-plane axis itself) this simply narrows the plane-sweep range;
// for axis 0/2 (Y is one of the mask's u/v dimensions instead) the mask's
// extent and the sample loop along that dimension both shrink to
// [yMin, yMax) rather than the full chunk height.
static void GreedyMeshAxis(const Chunk& chunk, const World& world, int axis, int yMin, int yMax,
        const TextureManager& textureManager, bool smoothLighting,
        std::vector<PackedVertex>& vertices, std::vector<uint32_t>& indices) {
    const int dims[3] = { CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z };
    int rangeStart[3] = { 0, yMin, 0 };
    int rangeEnd[3] = { dims[0], yMax, dims[2] };

    // `u`/`v` name the other two axes the mask sweeps across, and directly
    // become each quad's texture U/V direction.
    int u, v;
    if (axis == 0) { u = 2; v = 1; }
    else { u = (axis + 1) % 3; v = (axis + 2) % 3; }

    // Mask-local extent along u/v - full chunk width for a horizontal axis,
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

                // The sweep also visits the planes on this section's/chunk's
                // own outer boundary, where one of the two blocks is outside
                // [rangeStart, rangeEnd). The neighbor owns that block's face
                // and emits it in its own sweep, so skip it here
                bool aOutside = x[axis] < rangeStart[axis];
                bool bOutside = x[axis] + 1 >= rangeEnd[axis];

                if (aOpaque == bOpaque || (aOpaque && aOutside) || (bOpaque && bOutside)) {
                    mask[n] = {};
                } else {
                    // Air-side block of the face: b when a is the solid one, else a.
                    int pn = aOpaque ? x[axis] + 1 : x[axis];
                    mask[n] = {};
                    mask[n].type = aOpaque ? a.visualId : b.visualId;
                    mask[n].normal = aOpaque ? 1 : -1;
                    ComputeCornerShading(chunk, world, u, v, axis, x[u], x[v], pn, smoothLighting, mask[n]);
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

                    // Each corner takes its shading from the cell sitting in that corner of the quad.
                    int cellI = corners[c][0] ? i + w - 1 : i;
                    int cellJ = corners[c][1] ? j + h - 1 : j;
                    const MaskCell& corner = mask[cellI + cellJ * maskW];
                    int cu = corners[c][0], cv = corners[c][1];

                    vertices.push_back(PackedVertex::Pack(
                        glm::uvec3(localPos[0], localPos[1], localPos[2]),
                        static_cast<uint8_t>(faceIndex), // normalIndex, 0-5 fits in 3 bits.
                        corner.ao[cu][cv],
                        static_cast<uint8_t>(uOff), static_cast<uint8_t>(texV),
                        faceTexture.layer,
                        corner.blockLight[cu][cv],
                        corner.skyLight[cu][cv],
                        faceTexture.biomeTint,
                        faceTexture.shininess
                    ));
                }

                // Split along the diagonal joining the less occluded pair of
                // corners, so the AO gradient doesn't cut across a dark pair.
                int darkA = vertices[baseIndex + 0].word0 >> 24 & 0x3u;
                int darkB = vertices[baseIndex + 1].word0 >> 24 & 0x3u;
                int darkC = vertices[baseIndex + 2].word0 >> 24 & 0x3u;
                int darkD = vertices[baseIndex + 3].word0 >> 24 & 0x3u;
                if (darkA + darkC <= darkB + darkD) {
                    indices.insert(indices.end(), { baseIndex, baseIndex + 1, baseIndex + 2,
                        baseIndex + 2, baseIndex + 3, baseIndex });
                } else {
                    indices.insert(indices.end(), { baseIndex + 1, baseIndex + 2, baseIndex + 3,
                        baseIndex + 3, baseIndex, baseIndex + 1 });
                }

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

Mesh ChunkMesher::MeshChunkSection(const Chunk& chunk, const World& world, const TextureManager& textureManager, int sectionY) {
    std::vector<PackedVertex> vertices;
    std::vector<uint32_t> indices;

    int yMin = sectionY * SECTION_SIZE_Y;
    int yMax = yMin + SECTION_SIZE_Y;
    Config* config = Config::Active();
    bool smoothLighting = config ? std::get<bool>(config->Get("Graphics.SmoothLighting", true)) : true;
    for (int axis = 0; axis < 3; axis++) {
        GreedyMeshAxis(chunk, world, axis, yMin, yMax, textureManager, smoothLighting, vertices, indices);
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
        bool vertexOk = vertexSlab.Allocate(vertices.data(), vertexBufferSize, sizeof(PackedVertex), mesh.vertexBuffer, vertexOffset);
        if (vertexOk) {
            mesh.vertexOffset = vertexOffset;
            mesh.vertexBytes = vertexBufferSize;
        }

        VkDeviceSize indexBufferSize = sizeof(uint32_t) * indices.size();
        VkDeviceSize indexOffset = 0;
        bool indexOk = indexSlab.Allocate(indices.data(), indexBufferSize, sizeof(uint32_t), mesh.indexBuffer, indexOffset);
        if (indexOk) {
            mesh.indexOffset = indexOffset;
            mesh.indexBytes = indexBufferSize;
        }

        // Half a mesh is useless
        if (!vertexOk || !indexOk) {
            ReleaseMesh(mesh);
            mesh = Mesh{};
            mesh.allocationFailed = true;
        }
    }

    return mesh;
}

} // namespace Volcano
