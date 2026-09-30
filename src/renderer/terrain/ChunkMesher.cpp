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
        ? std::get<uint32_t>(config->Get("Graphics.SlabSize.MaxScalePercent", uint32_t{200}))
        : 200;

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
    // Per-axis [start, end) range actually swept — X/Z always full width;
    // only the axis that happens to be Y (whether that's the sweep axis
    // itself or one of the mask's u/v dimensions) is narrowed to the
    // section's own [yMin, yMax).
    int rangeStart[3] = { 0, yMin, 0 };
    int rangeEnd[3] = { dims[0], yMax, dims[2] };

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

    // Mask-local extent along u/v — full chunk width for a horizontal axis,
    // narrowed to the section height for Y (see rangeStart/rangeEnd above).
    int maskW = rangeEnd[u] - rangeStart[u];
    int maskH = rangeEnd[v] - rangeStart[v];
    std::vector<MaskCell> mask(static_cast<size_t>(maskW) * static_cast<size_t>(maskH));

    int x[3] = {0, 0, 0};
    int q[3] = {0, 0, 0};
    q[axis] = 1;

    // Planes run from rangeStart[axis]-1 (this slice's negative boundary)
    // through rangeEnd[axis]-1 (its positive boundary). Chunk::getBlock()
    // already clamps out-of-range coordinates to Air, so no special-casing
    // is needed at the chunk's own edges; a section-internal boundary
    // (rangeStart[axis] > 0, i.e. axis 1 above section 0) samples the real
    // block below it via GetBlockAcrossChunks/the chunk's own array, not Air
    // — sections aren't independently exposure-tested against each other,
    // only against real neighboring geometry.
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
                    // a is the solid block emitting this face; b (air) is the
                    // side it's actually exposed to, so light must be sampled
                    // there — a solid block's own stored light is always 0
                    // (light doesn't propagate into solid blocks), which is
                    // what made every face render pitch black.
                    LightSample light = GetLightAcrossChunks(chunk, world, x[0] + q[0], x[1] + q[1], x[2] + q[2]);
                    mask[n] = { a.visualId, 1, light.sky, light.block };
                } else {
                    LightSample light = GetLightAcrossChunks(chunk, world, x[0], x[1], x[2]);
                    mask[n] = { b.visualId, -1, light.sky, light.block };
                }
            }
        }

        x[axis]++; // x[axis] is now the coordinate of the plane the mask describes

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

                // Quad corners as (u, v) offsets from (x[u], x[v]); the winding
                // order differs by normal sign so the merged quad faces
                // outward (CCW seen from outside). positiveCorners walks
                // u then v, which is CCW around +axis only when u x v points
                // along +axis — true for Y (Z x X = +Y) and Z (X x Y = +Z),
                // but NOT for X, whose u/v were swapped above for upright
                // wall textures (Z x Y = -X). That one-axis handedness flip
                // is the "per-axis asymmetry" that made backface culling
                // look inconsistent before; X faces swap winding to match.
                static const int positiveCorners[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
                static const int negativeCorners[4][2] = { {0,0}, {0,1}, {1,1}, {1,0} };
                const bool handednessFlipped = (axis == 0);
                const auto& corners = ((cell.normal > 0) != handednessFlipped) ? positiveCorners : negativeCorners;

                uint32_t baseIndex = static_cast<uint32_t>(vertices.size());
                for (int c = 0; c < 4; c++) {
                    int uOff = corners[c][0] * w;
                    int vOff = corners[c][1] * h;

                    // Local position: 0-15 range on every axis, NOT world-
                    // space — chunk/section offset is applied via the model
                    // matrix instead. Y specifically comes out of x[1] in
                    // [rangeStart[1], rangeEnd[1]) (absolute chunk-local,
                    // e.g. 32-47 for section 2) — subtract rangeStart[1]
                    // (yMin) to make it section-relative (0-15) before
                    // packing, matching MeshChunkSection's own model-matrix
                    // translation to that section's Y origin.
                    int localPos[3] = { x[0], x[1], x[2] };
                    localPos[u] = x[u] + uOff;
                    localPos[v] = x[v] + vOff;
                    localPos[1] -= yMin;

                    // Texture V runs DOWN the image (V=0 is its top row), but
                    // vOff runs UP the world on the two side axes — v is Y
                    // there (see the u/v assignment above). Feeding vOff
                    // straight through therefore put the BOTTOM of a wall at
                    // the TOP of its texture, which is exactly why
                    // grass_block_side (and every other side texture) rendered
                    // upside down. Flipping it lands V=0 on the top edge of
                    // the quad. Y-axis faces (floor/ceiling, where v is X) have
                    // no up/down to get wrong, so they pass through unchanged.
                    int texV = (v == 1) ? (h - vOff) : vOff;

                    // UV is packed as raw block-space tile counts (not normalized
                    // 0-1) so a merged quad tiles its texture `w` x `h` times via
                    // the texture sampler's REPEAT wrap mode, rather than
                    // stretching one texture across the whole rect.
                    vertices.push_back(PackedVertex::Pack(
                        glm::uvec3(localPos[0], localPos[1], localPos[2]),
                        static_cast<uint8_t>(faceIndex), // normalIndex, 0-5 fits in 3 bits
                        0,                     // AO — not computed yet
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
