#include "NonCubicMesher.hpp"
#include "MiscVertex.hpp"
#include "../terrain/models/BlockRegistry.hpp"
#include "../terrain/models/ChunkSection.hpp"
#include "../models/SlabBuffer.hpp"
#include "../VulkanInit.hpp"
#include "../BiomeColors.hpp"
#include "../../Logger.hpp"
#include "../../Config.hpp"
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace Volcano {

// Same "one slab pair, bump-allocated per chunk mesh" scheme ChunkMesher
// uses - see its own comment for why (streamed-in chunks, avoiding a
// VkBuffer/VmaAllocation per chunk). Kept entirely separate from
// ChunkMesher's slabs since this mesh is bound by different pipelines
// (nonCubicPipeline/translucentPipeline) with a different vertex format
// (MiscVertex). Two pairs - cutout and translucent - since MeshChunk now
// produces two Meshes per chunk; see NonCubicMesher::ChunkMeshes.
static SlabBuffer vertexSlab;
static SlabBuffer indexSlab;
static SlabBuffer translucentVertexSlab;
static SlabBuffer translucentIndexSlab;
static bool slabsInitialized = false;

// Scales this mesher's base slab size by how many chunk columns
// Graphics.RenderDistance actually loads - (2*RD+1)^2, the same square-view
// accounting vanilla's own simulation distance uses - rather than a single
// fixed budget that's only right for whatever distance it happened to be
// tuned at (128 MB overflows constantly at a render distance as small as
// 6). Every factor lives in settings.json (Graphics.SlabSize.*) rather than
// being hardcoded, since the right numbers depend entirely on the machine:
// this is a single VkDeviceMemory allocation out of the host-visible heap
// SlabBuffer maps persistently (CreateBuffer's isUMA flag), which on an
// integrated/UMA GPU is real system RAM, and on a discrete GPU without
// resizable BAR is often capped in the low hundreds of MB regardless of
// total VRAM - scaling too aggressively risks that allocation failing
// outright (SlabBuffer::Init now throws a readable error instead of
// crashing on that, but a smaller increase that stays reliable beats a
// bigger one that doesn't). If it's still failing, lower baseSizeMBKey
// and/or maxScalePercent in settings.json.
//
// baseSizeMBKey: the base size in MB (floor - never allocates less than
// this regardless of render distance). ReferenceRenderDistance: the render
// distance that base size is considered "right" for; scale is 1.0 there.
// MaxScalePercent: hard cap on how far render distance can scale the base
// size up (percent, not a fraction - ConfigValue has no float type, same
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

    uint64_t slabSize = SlabSizeForRenderDistance("Graphics.SlabSize.NonCubicMB",
        static_cast<uint32_t>(NonCubicMesher::DEFAULT_SLAB_SIZE / (1024ull * 1024ull)));
    vertexSlab.Init(slabSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    indexSlab.Init(slabSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    translucentVertexSlab.Init(slabSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    translucentIndexSlab.Init(slabSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    slabsInitialized = true;
}

void NonCubicMesher::Shutdown() {
    if (!slabsInitialized) return;

    vertexSlab.Destroy();
    indexSlab.Destroy();
    translucentVertexSlab.Destroy();
    translucentIndexSlab.Destroy();
    slabsInitialized = false;
}

bool NonCubicMesher::ReleaseMesh(const Mesh& mesh) {
    if (!slabsInitialized) return false;
    bool owned = false;
    for (SlabBuffer* slab : { &vertexSlab, &translucentVertexSlab }) {
        if (mesh.vertexBytes > 0 && slab->Owns(mesh.vertexBuffer)) {
            slab->Free(mesh.vertexBuffer, mesh.vertexOffset, mesh.vertexBytes);
            owned = true;
        }
    }
    for (SlabBuffer* slab : { &indexSlab, &translucentIndexSlab }) {
        if (mesh.indexBytes > 0 && slab->Owns(mesh.indexBuffer)) {
            slab->Free(mesh.indexBuffer, mesh.indexOffset, mesh.indexBytes);
            owned = true;
        }
    }
    return owned;
}

SlabStats NonCubicMesher::GetSlabStats() {
    return SlabStats{
        vertexSlab.GetUsed() + indexSlab.GetUsed()
            + translucentVertexSlab.GetUsed() + translucentIndexSlab.GetUsed(),
        vertexSlab.GetCapacity() + indexSlab.GetCapacity()
            + translucentVertexSlab.GetCapacity() + translucentIndexSlab.GetCapacity(),
        vertexSlab.GetLimit() + indexSlab.GetLimit()
            + translucentVertexSlab.GetLimit() + translucentIndexSlab.GetLimit(),
        vertexSlab.GetBlockCount() + indexSlab.GetBlockCount()
            + translucentVertexSlab.GetBlockCount() + translucentIndexSlab.GetBlockCount()
    };
}

// Neighbor offset for each of ChunkMesher's 6 face indices - used both to
// cull a transparent-cube face against an identical neighbor (see
// MeshTransparentCube) and to sample the real light a face is exposed to.
static constexpr int FACE_DX[6] = { 0, 0, 0, 0, 1, -1 };
static constexpr int FACE_DY[6] = { 1, -1, 0, 0, 0, 0 };
static constexpr int FACE_DZ[6] = { 0, 0, -1, 1, 0, 0 };

// Reads a block that may be outside this chunk's own 0..CHUNK_SIZE_X-1 /
// 0..CHUNK_SIZE_Z-1 horizontal range by falling back to World for anything
// that crosses into a neighboring chunk - same reasoning and shape as
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
// exposed to (see ChunkMesher's own GetLightAcrossChunks). Both channels
// from one lookup - see LightSample's own comment.
static LightSample GetLightAcrossChunks(const Chunk& chunk, const World& world, int lx, int ly, int lz) {
    if (lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z) {
        return chunk.getLightSample(lx, ly, lz);
    }

    int worldX = chunk.getX() * CHUNK_SIZE_X + lx;
    int worldZ = chunk.getZ() * CHUNK_SIZE_Z + lz;
    return world.GetLightSample(worldX, ly + WORLD_MIN_Y, worldZ);
}

// Four corners of one face of an axis-aligned box, in some consistent
// (not necessarily outward-CCW) winding - the non-cubic pipeline disables
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
// model JSON's own "uv" - see NonCubeElement::faceUVs), normalized here to
// 0..1. Corner order matches FaceCorners' - (u1,v1) at corner 0 through
// (u1,v2) at corner 3 - the same parametrization a full 0,0,16,16 rect
// degenerates to, so this is a strict generalization of the old hardcoded
// full-texture UVs, not a behavior change for anything that already used
// the whole texture (full cubes, cross planes).
//
// `verticalFace` flips V, for the same reason ChunkMesher's own texV does:
// texture V runs DOWN the image (V=0 is its top row), while every vertical
// quad this file emits is built with corners 0/1 along its BOTTOM edge and
// 2/3 along its top (see FaceCorners' four side cases, and MeshCross's own
// corner list). Handing those corners an unflipped V therefore pinned the
// top of the texture to the bottom of the quad - the same upside-down side
// textures the terrain pass had. True for the four side faces (2-5) and for
// cross billboards; false for Up/Down faces, which are horizontal and have
// no up/down to get wrong.
static void EmitQuad(std::vector<MiscVertex>& vertices, std::vector<uint32_t>& indices,
        const std::array<glm::vec3, 4>& corners, const glm::vec4& uvRect,
        uint16_t textureLayer, BiomeTint tint, LightSample light, bool verticalFace) {
    glm::vec2 uvMin = glm::vec2(uvRect.x, uvRect.y) / 16.0f;
    glm::vec2 uvMax = glm::vec2(uvRect.z, uvRect.w) / 16.0f;
    float vBottom = verticalFace ? uvMax.y : uvMin.y;
    float vTop = verticalFace ? uvMin.y : uvMax.y;
    const std::array<glm::vec2, 4> uvs = {
        glm::vec2(uvMin.x, vBottom), glm::vec2(uvMax.x, vBottom),
        glm::vec2(uvMax.x, vTop), glm::vec2(uvMin.x, vTop)
    };

    uint32_t base = static_cast<uint32_t>(vertices.size());
    uint32_t packed = MiscVertex::Pack(textureLayer, light.sky, light.block, static_cast<uint8_t>(tint));

    for (int i = 0; i < 4; i++) {
        vertices.push_back(MiscVertex{ corners[static_cast<size_t>(i)], uvs[static_cast<size_t>(i)], packed });
    }

    indices.insert(indices.end(), { base, base + 1, base + 2, base + 2, base + 3, base });
}

// True when element's face f lies exactly on the block's 0..16 boundary
// along the axis its normal points - meaning an opaque neighbor there would
// fully occlude it, since the neighbor's own face spans the full 16x16 area
// on that plane regardless of how much of it this element's own face
// actually covers. Same condition ChunkMesher's greedy sweep uses to cull
// between two opaque blocks, just checked per-element instead of per-voxel.
// A face that ISN'T flush (a torch's stick, the inner corner of a stair's
// raised step) never gets culled - nothing on the other side of it is ever
// a full, coplanar occluder.
static bool IsFaceFlush(const BlockRegistry::NonCubeElement& element, int face) {
    constexpr float EPS = 0.01f;
    switch (face) {
        case 0: return element.to.y > 16.0f - EPS;   // Up
        case 1: return element.from.y < EPS;         // Down
        case 2: return element.from.z < EPS;         // North
        case 3: return element.to.z > 16.0f - EPS;   // South
        case 4: return element.to.x > 16.0f - EPS;   // East
        default: return element.from.x < EPS;        // West
    }
}

// Emits every present face of one model element (a box in 0..16 model-space
// units), skipping any face IsFaceFlush() above says is fully hidden behind
// a solid neighbor - without this, a stair's base slab flickered against the
// solid block it sits on (both drew a face on the exact same plane, and with
// this pass's depth WRITE off - see VulkanInit's nonCubicPipeline - neither
// consistently "won," so which one showed varied frame to frame), and same
// for a slab's top face against whatever's placed directly above it. A face
// that isn't flush (a torch's thin stick, a stair step's inner corner) is
// never culled, matching vanilla's own per-face "cullface" hint in the model
// JSON (not read directly here, but this reproduces the same cases).
static void MeshElementFaces(const BlockRegistry::NonCubeElement& element, const Chunk& chunk, const World& world,
        int bx, int by, int bz, const glm::vec3& blockOrigin, const TextureManager& textureManager,
        std::vector<MiscVertex>& vertices, std::vector<uint32_t>& indices) {
    glm::vec3 boxMin = blockOrigin + element.from / 16.0f;
    glm::vec3 boxMax = blockOrigin + element.to / 16.0f;

    for (int f = 0; f < 6; f++) {
        const std::string& textureName = element.faceTextures[static_cast<size_t>(f)];
        if (textureName.empty()) continue;

        if (IsFaceFlush(element, f)) {
            Block neighbor = GetBlockAcrossChunks(chunk, world, bx + FACE_DX[f], by + FACE_DY[f], bz + FACE_DZ[f]);
            if (neighbor.isOpaque()) continue;
        }

        uint16_t layer = textureManager.GetLayerIndex(textureName);
        LightSample light = GetLightAcrossChunks(chunk, world, bx + FACE_DX[f], by + FACE_DY[f], bz + FACE_DZ[f]);
        EmitQuad(vertices, indices, FaceCorners(boxMin, boxMax, f), element.faceUVs[static_cast<size_t>(f)],
            layer, GetMiscTint(textureName), light, f >= 2);
    }
}

// Transparent full cubes (glass, slime, ice, ...) skip a face when the
// immediate neighbor shares the exact same non-cube visual (same-type glass
// panes don't show their shared internal face) OR is a fully opaque block
// (every face of a full cube is flush by construction, so an opaque neighbor
// there is the exact same coplanar-face flicker IsFaceFlush/MeshElementFaces
// above fixes for partial shapes - glass placed directly against stone used
// to flicker at that boundary the same way). Any other neighbor (air, a
// different transparent visual) still draws the face.
static void MeshTransparentCube(const BlockRegistry::NonCubeElement& element, uint16_t visualId,
        const Chunk& chunk, const World& world, int bx, int by, int bz, const glm::vec3& blockOrigin,
        const TextureManager& textureManager, std::vector<MiscVertex>& vertices, std::vector<uint32_t>& indices) {
    glm::vec3 boxMin = blockOrigin + element.from / 16.0f;
    glm::vec3 boxMax = blockOrigin + element.to / 16.0f;

    for (int f = 0; f < 6; f++) {
        const std::string& textureName = element.faceTextures[static_cast<size_t>(f)];
        if (textureName.empty()) continue;

        Block neighbor = GetBlockAcrossChunks(chunk, world, bx + FACE_DX[f], by + FACE_DY[f], bz + FACE_DZ[f]);
        if (neighbor.nonCubeVisualId == visualId || neighbor.isOpaque()) continue;

        uint16_t layer = textureManager.GetLayerIndex(textureName);
        LightSample light = GetLightAcrossChunks(chunk, world, bx + FACE_DX[f], by + FACE_DY[f], bz + FACE_DZ[f]);
        EmitQuad(vertices, indices, FaceCorners(boxMin, boxMax, f), element.faceUVs[static_cast<size_t>(f)],
            layer, GetMiscTint(textureName), light, f >= 2);
    }
}

// Fluids, drawn the way vanilla's FluidRenderer does. Each top corner takes
// the weighted average of the four cells around it (full-height neighbors
// dominate, solids don't count), and the fluid reads as full height under
// more of itself, so neighboring surfaces join into one slope. Side faces
// span the two corner heights beside them.
struct FluidCell {
    uint8_t kind = 0;   // 0 = not a fluid, 1 = water, 2 = lava.
    float height = 0.0f;
    bool solid = false;
};

static FluidCell FluidAt(const Chunk& chunk, const World& world, int x, int y, int z) {
    Block block = GetBlockAcrossChunks(chunk, world, x, y, z);
    FluidCell cell;
    if (block.nonCubeVisualId != 0) {
        const BlockRegistry::NonCubeVisual& visual = BlockRegistry::GetNonCubeVisual(block.nonCubeVisualId);
        if (visual.shape == BlockRegistry::NonCubeShape::Liquid) {
            cell.kind = visual.fluidKind;
            cell.height = visual.fluidHeight;
            return cell;
        }
    }
    cell.solid = block.hasCollision();
    return cell;
}

// -1 for a solid cell, 0 for open air, otherwise the fluid's surface height.
static float FluidHeightAt(uint8_t kind, const Chunk& chunk, const World& world, int x, int y, int z) {
    if (FluidAt(chunk, world, x, y + 1, z).kind == kind) return 1.0f;
    FluidCell cell = FluidAt(chunk, world, x, y, z);
    if (cell.kind == kind) return cell.height;
    return cell.solid ? -1.0f : 0.0f;
}

static float FluidCornerHeight(uint8_t kind, const Chunk& chunk, const World& world,
        float own, float sideA, float sideB, int diagonalX, int diagonalY, int diagonalZ) {
    if (sideA >= 1.0f || sideB >= 1.0f) return 1.0f;

    float total = 0.0f, weight = 0.0f;
    auto add = [&](float height) {
        if (height >= 0.8f) { total += height * 10.0f; weight += 10.0f; }
        else if (height >= 0.0f) { total += height; weight += 1.0f; }
    };
    if (sideA > 0.0f || sideB > 0.0f) {
        float diagonal = FluidHeightAt(kind, chunk, world, diagonalX, diagonalY, diagonalZ);
        if (diagonal >= 1.0f) return 1.0f;
        add(diagonal);
    }
    add(own);
    add(sideA);
    add(sideB);
    return weight > 0.0f ? total / weight : 0.0f;
}

static void MeshFluid(const BlockRegistry::NonCubeVisual& visual, const Chunk& chunk, const World& world,
        int bx, int by, int bz, const glm::vec3& blockOrigin, const TextureManager& textureManager,
        std::vector<MiscVertex>& vertices, std::vector<uint32_t>& indices) {
    const uint8_t kind = visual.fluidKind;
    const bool lava = kind == 2;
    uint16_t layer = textureManager.GetLayerIndex(lava ? "lava_still" : "water_still");
    BiomeTint tint = lava ? BiomeTint::None : BiomeTint::Water;
    auto lightAt = [&](int x, int y, int z) {
        if (lava) return LightSample{ 15, 15 }; // Lava glows at full brightness.
        return GetLightAcrossChunks(chunk, world, x, y, z);
    };
    auto hidden = [&](int x, int y, int z) {
        Block block = GetBlockAcrossChunks(chunk, world, x, y, z);
        if (block.isOpaque()) return true;
        return FluidAt(chunk, world, x, y, z).kind == kind;
    };
    const glm::vec4 fullUV(0.0f, 0.0f, 16.0f, 16.0f);

    float own = FluidHeightAt(kind, chunk, world, bx, by, bz);
    float north = FluidHeightAt(kind, chunk, world, bx, by, bz - 1);
    float south = FluidHeightAt(kind, chunk, world, bx, by, bz + 1);
    float east = FluidHeightAt(kind, chunk, world, bx + 1, by, bz);
    float west = FluidHeightAt(kind, chunk, world, bx - 1, by, bz);

    // Same fluid above: this cell is full height with no visible top.
    const bool submerged = FluidAt(chunk, world, bx, by + 1, bz).kind == kind;
    float hNW = 1.0f, hSW = 1.0f, hSE = 1.0f, hNE = 1.0f;
    if (!submerged)
    {
        hNW = FluidCornerHeight(kind, chunk, world, own, north, west, bx - 1, by, bz - 1);
        hSW = FluidCornerHeight(kind, chunk, world, own, south, west, bx - 1, by, bz + 1);
        hSE = FluidCornerHeight(kind, chunk, world, own, south, east, bx + 1, by, bz + 1);
        hNE = FluidCornerHeight(kind, chunk, world, own, north, east, bx + 1, by, bz - 1);
    }

    const glm::vec3 min = blockOrigin, max = blockOrigin + glm::vec3(1.0f);
    float y0 = min.y;

    if (!hidden(bx, by + 1, bz)) {
        std::array<glm::vec3, 4> top = {
            glm::vec3(min.x, y0 + hNW, min.z), glm::vec3(max.x, y0 + hNE, min.z),
            glm::vec3(max.x, y0 + hSE, max.z), glm::vec3(min.x, y0 + hSW, max.z) };
        EmitQuad(vertices, indices, top, fullUV, layer, tint, lightAt(bx, by + 1, bz), false);
    }
    if (!hidden(bx, by - 1, bz)) {
        EmitQuad(vertices, indices, FaceCorners(min, glm::vec3(max.x, min.y, max.z), 1), fullUV, layer, tint,
            lightAt(bx, by - 1, bz), false);
    }

    struct Side { int dx, dz; std::array<glm::vec3, 4> corners; float topHeight; };
    Side sides[4] = {
        { 0, -1, { glm::vec3(max.x, y0, min.z), glm::vec3(min.x, y0, min.z),
                   glm::vec3(min.x, y0 + hNW, min.z), glm::vec3(max.x, y0 + hNE, min.z) }, std::max(hNW, hNE) },
        { 0, 1, { glm::vec3(min.x, y0, max.z), glm::vec3(max.x, y0, max.z),
                  glm::vec3(max.x, y0 + hSE, max.z), glm::vec3(min.x, y0 + hSW, max.z) }, std::max(hSE, hSW) },
        { 1, 0, { glm::vec3(max.x, y0, max.z), glm::vec3(max.x, y0, min.z),
                  glm::vec3(max.x, y0 + hNE, min.z), glm::vec3(max.x, y0 + hSE, max.z) }, std::max(hNE, hSE) },
        { -1, 0, { glm::vec3(min.x, y0, min.z), glm::vec3(min.x, y0, max.z),
                   glm::vec3(min.x, y0 + hSW, max.z), glm::vec3(min.x, y0 + hNW, min.z) }, std::max(hSW, hNW) },
    };
    for (const Side& side : sides) {
        if (hidden(bx + side.dx, by, bz + side.dz)) continue;
        glm::vec4 uv(0.0f, 0.0f, 16.0f, 16.0f * std::max(side.topHeight, 0.05f));
        EmitQuad(vertices, indices, side.corners, uv, layer, tint, lightAt(bx + side.dx, by, bz + side.dz), true);
    }
}

// N billboard planes through the block's center, evenly spaced across 180°
// (each plane is visible from both sides - the non-cubic pipeline disables
// backface culling - so 180° of spacing gives N crossing planes, matching
// vanilla's 2-plane X-shape at CROSS_BILLBOARD_COUNT's default of 2: 45° and
// 135°, the same pair of diagonals vanilla's block/cross model uses).
static void MeshCross(const BlockRegistry::NonCubeVisual& visual, const Chunk& chunk,
        int bx, int by, int bz, const glm::vec3& blockOrigin, const TextureManager& textureManager,
        std::vector<MiscVertex>& vertices, std::vector<uint32_t>& indices) {
    uint16_t layer = textureManager.GetLayerIndex(visual.crossTexture);
    BiomeTint tint = GetMiscTint(visual.crossTexture);
    // Flat - a billboard has no single well-defined face normal to sample a neighbor by; use the block's own light.
    LightSample light = chunk.getLightSample(bx, by, bz);

    glm::vec3 center = blockOrigin + glm::vec3(0.5f, 0.0f, 0.5f);
    int planeCount = CROSS_BILLBOARD_COUNT > 0 ? CROSS_BILLBOARD_COUNT : 1;

    for (int i = 0; i < planeCount; i++) {
        float angle = glm::radians(45.0f + 180.0f * static_cast<float>(i) / static_cast<float>(planeCount));
        glm::vec3 half = glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * 0.5f;

        glm::vec3 p0 = center - half;
        glm::vec3 p1 = center + half;

        std::array<glm::vec3, 4> corners = {
            glm::vec3(p0.x, blockOrigin.y, p0.z),
            glm::vec3(p1.x, blockOrigin.y, p1.z),
            glm::vec3(p1.x, blockOrigin.y + 1.0f, p1.z),
            glm::vec3(p0.x, blockOrigin.y + 1.0f, p0.z),
        };
        EmitQuad(vertices, indices, corners, glm::vec4(0.0f, 0.0f, 16.0f, 16.0f), layer, tint, light,
            /* verticalFace */ true);
    }
}

// Bump-allocates one Mesh's vertices/indices into the given slab pair -
// shared by MeshChunk's two outputs (cutout and translucent) below so the
// allocate-or-fall-back-to-empty logic only exists once.
static Mesh BuildMesh(const std::vector<MiscVertex>& vertices, const std::vector<uint32_t>& indices,
        const glm::mat4& modelMatrix, SlabBuffer& targetVertexSlab, SlabBuffer& targetIndexSlab) {
    Mesh mesh{};
    mesh.vertexCount = static_cast<uint32_t>(vertices.size());
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.modelMatrix = modelMatrix;

    if (mesh.vertexCount > 0) {
        EnsureSlabsInitialized();

        VkDeviceSize vertexBufferSize = sizeof(MiscVertex) * vertices.size();
        VkDeviceSize vertexOffset = 0;
        bool vertexOk = targetVertexSlab.Allocate(vertices.data(), vertexBufferSize, sizeof(MiscVertex), mesh.vertexBuffer, vertexOffset);
        if (vertexOk) {
            mesh.vertexOffset = vertexOffset;
            mesh.vertexBytes = vertexBufferSize;
        }

        VkDeviceSize indexBufferSize = sizeof(uint32_t) * indices.size();
        VkDeviceSize indexOffset = 0;
        bool indexOk = targetIndexSlab.Allocate(indices.data(), indexBufferSize, sizeof(uint32_t), mesh.indexBuffer, indexOffset);
        if (indexOk) {
            mesh.indexOffset = indexOffset;
            mesh.indexBytes = indexBufferSize;
        }

        // Half a mesh is useless - give back whichever half did fit, and flag
        // the failure so the caller keeps the section's old mesh and retries
        // (see ChunkMesher.cpp's own comment).
        if (!vertexOk || !indexOk) {
            NonCubicMesher::ReleaseMesh(mesh);
            mesh = Mesh{};
            mesh.allocationFailed = true;
        }
    }

    return mesh;
}

NonCubicMesher::ChunkMeshes NonCubicMesher::MeshChunkSection(const Chunk& chunk, const World& world, const TextureManager& textureManager, int sectionY) {
    std::vector<MiscVertex> cutoutVertices;
    std::vector<uint32_t> cutoutIndices;
    std::vector<MiscVertex> translucentVertices;
    std::vector<uint32_t> translucentIndices;

    int yMin = sectionY * SECTION_SIZE_Y;
    int yMax = yMin + SECTION_SIZE_Y;
    for (int y = yMin; y < yMax; y++) {
        for (int z = 0; z < CHUNK_SIZE_Z; z++) {
            for (int x = 0; x < CHUNK_SIZE_X; x++) {
                Block block = chunk.getBlock(x, y, z);
                if (block.nonCubeVisualId == 0) continue;

                const BlockRegistry::NonCubeVisual& visual = BlockRegistry::GetNonCubeVisual(block.nonCubeVisualId);
                // blockOrigin.y is section-relative (0-15), matching the
                // model matrix's own translation to this section's Y origin
                // below - bx/by/bz passed to the Mesh* helpers below stay
                // absolute chunk-local (their own neighbor lookups need the
                // real coordinate), only the vertex-position base changes.
                glm::vec3 blockOrigin(static_cast<float>(x), static_cast<float>(y - yMin), static_cast<float>(z));

                if (visual.shape == BlockRegistry::NonCubeShape::Cross) {
                    MeshCross(visual, chunk, x, y, z, blockOrigin, textureManager, cutoutVertices, cutoutIndices);
                    continue;
                }

                if (visual.shape == BlockRegistry::NonCubeShape::Liquid) {
                    // Lava is opaque; water alpha-blends.
                    if (visual.fluidKind == 2) MeshFluid(visual, chunk, world, x, y, z, blockOrigin, textureManager, cutoutVertices, cutoutIndices);
                    else MeshFluid(visual, chunk, world, x, y, z, blockOrigin, textureManager, translucentVertices, translucentIndices);
                    continue;
                }

                for (const BlockRegistry::NonCubeElement& element : visual.elements) {
                    if (visual.shape == BlockRegistry::NonCubeShape::TransparentCube) {
                        MeshTransparentCube(element, block.nonCubeVisualId, chunk, world, x, y, z, blockOrigin,
                            textureManager, cutoutVertices, cutoutIndices);
                    } else {
                        MeshElementFaces(element, chunk, world, x, y, z, blockOrigin, textureManager, cutoutVertices, cutoutIndices);
                    }
                }
            }
        }
    }

    glm::mat4 modelMatrix = glm::translate(glm::mat4(1.0f),
        glm::vec3(chunk.getX() * CHUNK_SIZE_X, WORLD_MIN_Y + yMin, chunk.getZ() * CHUNK_SIZE_Z));

    ChunkMeshes result;
    result.cutout = BuildMesh(cutoutVertices, cutoutIndices, modelMatrix, vertexSlab, indexSlab);
    result.translucent = BuildMesh(translucentVertices, translucentIndices, modelMatrix,
        translucentVertexSlab, translucentIndexSlab);
    return result;
}

} // namespace Volcano
