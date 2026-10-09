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
// uses — see its own comment for why (streamed-in chunks, avoiding a
// VkBuffer/VmaAllocation per chunk). Kept entirely separate from
// ChunkMesher's slabs since this mesh is bound by different pipelines
// (nonCubicPipeline/translucentPipeline) with a different vertex format
// (MiscVertex). Two pairs — cutout and translucent — since MeshChunk now
// produces two Meshes per chunk; see NonCubicMesher::ChunkMeshes.
static SlabBuffer vertexSlab;
static SlabBuffer indexSlab;
static SlabBuffer translucentVertexSlab;
static SlabBuffer translucentIndexSlab;
static bool slabsInitialized = false;

// Scales this mesher's base slab size by how many chunk columns
// Graphics.RenderDistance actually loads — (2*RD+1)^2, the same square-view
// accounting vanilla's own simulation distance uses — rather than a single
// fixed budget that's only right for whatever distance it happened to be
// tuned at (128 MB overflows constantly at a render distance as small as
// 6). Every factor lives in settings.json (Graphics.SlabSize.*) rather than
// being hardcoded, since the right numbers depend entirely on the machine:
// this is a single VkDeviceMemory allocation out of the host-visible heap
// SlabBuffer maps persistently (CreateBuffer's isUMA flag), which on an
// integrated/UMA GPU is real system RAM, and on a discrete GPU without
// resizable BAR is often capped in the low hundreds of MB regardless of
// total VRAM — scaling too aggressively risks that allocation failing
// outright (SlabBuffer::Init now throws a readable error instead of
// crashing on that, but a smaller increase that stays reliable beats a
// bigger one that doesn't). If it's still failing, lower baseSizeMBKey
// and/or maxScalePercent in settings.json.
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

    uint64_t slabSize = SlabSizeForRenderDistance("Graphics.SlabSize.NonCubicMB",
        static_cast<uint32_t>(NonCubicMesher::DEFAULT_SLAB_SIZE / (1024ull * 1024ull)));
    slabSize = SlabBuffer::AffordableSize(slabSize, 0.3, 4); // 4 slabs, 30% of the slab budget
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
        if (mesh.vertexBytes > 0 && mesh.vertexBuffer == slab->GetBuffer()) {
            slab->Free(mesh.vertexOffset, mesh.vertexBytes);
            owned = true;
        }
    }
    for (SlabBuffer* slab : { &indexSlab, &translucentIndexSlab }) {
        if (mesh.indexBytes > 0 && mesh.indexBuffer == slab->GetBuffer()) {
            slab->Free(mesh.indexOffset, mesh.indexBytes);
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
            + translucentVertexSlab.GetCapacity() + translucentIndexSlab.GetCapacity()
    };
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
// exposed to (see ChunkMesher's own GetLightAcrossChunks). Both channels
// from one lookup — see LightSample's own comment.
static LightSample GetLightAcrossChunks(const Chunk& chunk, const World& world, int lx, int ly, int lz) {
    if (lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z) {
        return chunk.getLightSample(lx, ly, lz);
    }

    int worldX = chunk.getX() * CHUNK_SIZE_X + lx;
    int worldZ = chunk.getZ() * CHUNK_SIZE_Z + lz;
    return world.GetLightSample(worldX, ly + WORLD_MIN_Y, worldZ);
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
//
// `verticalFace` flips V, for the same reason ChunkMesher's own texV does:
// texture V runs DOWN the image (V=0 is its top row), while every vertical
// quad this file emits is built with corners 0/1 along its BOTTOM edge and
// 2/3 along its top (see FaceCorners' four side cases, and MeshCross's own
// corner list). Handing those corners an unflipped V therefore pinned the
// top of the texture to the bottom of the quad — the same upside-down side
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
// along the axis its normal points — meaning an opaque neighbor there would
// fully occlude it, since the neighbor's own face spans the full 16x16 area
// on that plane regardless of how much of it this element's own face
// actually covers. Same condition ChunkMesher's greedy sweep uses to cull
// between two opaque blocks, just checked per-element instead of per-voxel.
// A face that ISN'T flush (a torch's stick, the inner corner of a stair's
// raised step) never gets culled — nothing on the other side of it is ever
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
// a solid neighbor — without this, a stair's base slab flickered against the
// solid block it sits on (both drew a face on the exact same plane, and with
// this pass's depth WRITE off — see VulkanInit's nonCubicPipeline — neither
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
// above fixes for partial shapes — glass placed directly against stone used
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

// Water's own face culling: skip a face when the neighbor is ALSO water, at
// ANY level (mirrors vanilla's FluidRenderer.isNeighborSameFluid — a shared
// water/water face never draws regardless of the two levels either side of
// it, sloped-corner interpolation or not), or when it's occluded by a flush
// opaque neighbor (same rule MeshElementFaces uses for slabs/carpets/etc.).
// Emits into the caller's own vertex/index vectors — MeshChunk below routes
// this shape's output to the translucent pass instead of the cutout one.
static bool IsWaterVisual(uint16_t nonCubeVisualId) {
    return nonCubeVisualId != 0
        && BlockRegistry::GetNonCubeVisual(nonCubeVisualId).shape == BlockRegistry::NonCubeShape::Liquid;
}

static void MeshLiquid(const BlockRegistry::NonCubeElement& element, const Chunk& chunk, const World& world,
        int bx, int by, int bz, const glm::vec3& blockOrigin, const TextureManager& textureManager,
        std::vector<MiscVertex>& vertices, std::vector<uint32_t>& indices) {
    glm::vec3 boxMin = blockOrigin + element.from / 16.0f;
    glm::vec3 boxMax = blockOrigin + element.to / 16.0f;

    for (int f = 0; f < 6; f++) {
        const std::string& textureName = element.faceTextures[static_cast<size_t>(f)];
        if (textureName.empty()) continue;

        Block neighbor = GetBlockAcrossChunks(chunk, world, bx + FACE_DX[f], by + FACE_DY[f], bz + FACE_DZ[f]);
        if (IsWaterVisual(neighbor.nonCubeVisualId)) continue;
        if (IsFaceFlush(element, f) && neighbor.isOpaque()) continue;

        uint16_t layer = textureManager.GetLayerIndex(textureName);
        LightSample light = GetLightAcrossChunks(chunk, world, bx + FACE_DX[f], by + FACE_DY[f], bz + FACE_DZ[f]);
        EmitQuad(vertices, indices, FaceCorners(boxMin, boxMax, f), element.faceUVs[static_cast<size_t>(f)],
            layer, BiomeTint::Water, light, f >= 2);
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
    BiomeTint tint = GetMiscTint(visual.crossTexture);
    // Flat — a billboard has no single well-defined face normal to sample a neighbor by; use the block's own light.
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

// Bump-allocates one Mesh's vertices/indices into the given slab pair —
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
        if (targetVertexSlab.Allocate(vertices.data(), vertexBufferSize, sizeof(MiscVertex), vertexOffset)) {
            mesh.vertexBuffer = targetVertexSlab.GetBuffer();
            mesh.vertexOffset = vertexOffset;
            mesh.vertexBytes = vertexBufferSize;
        } else {
            // See ChunkMesher.cpp's own Allocate-failure comment.
            glm::vec3 origin(modelMatrix[3]);
            Log::Error("[MESH] non-cubic vertex slab full (near " + std::to_string(origin.x) + ","
                + std::to_string(origin.y) + "," + std::to_string(origin.z) + ") — section not updated.");
            mesh.vertexCount = 0;
        }

        VkDeviceSize indexBufferSize = sizeof(uint32_t) * indices.size();
        VkDeviceSize indexOffset = 0;
        if (targetIndexSlab.Allocate(indices.data(), indexBufferSize, sizeof(uint32_t), indexOffset)) {
            mesh.indexBuffer = targetIndexSlab.GetBuffer();
            mesh.indexOffset = indexOffset;
            mesh.indexBytes = indexBufferSize;
        } else {
            glm::vec3 origin(modelMatrix[3]);
            Log::Error("[MESH] non-cubic index slab full (near " + std::to_string(origin.x) + ","
                + std::to_string(origin.y) + "," + std::to_string(origin.z) + ") — section not updated.");
            mesh.indexCount = 0;
        }

        // Half a mesh is useless — give back whichever half did fit.
        if (mesh.vertexCount == 0 || mesh.indexCount == 0) {
            NonCubicMesher::ReleaseMesh(mesh);
            mesh = Mesh{};
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
                // below — bx/by/bz passed to the Mesh* helpers below stay
                // absolute chunk-local (their own neighbor lookups need the
                // real coordinate), only the vertex-position base changes.
                glm::vec3 blockOrigin(static_cast<float>(x), static_cast<float>(y - yMin), static_cast<float>(z));

                if (visual.shape == BlockRegistry::NonCubeShape::Cross) {
                    MeshCross(visual, chunk, x, y, z, blockOrigin, textureManager, cutoutVertices, cutoutIndices);
                    continue;
                }

                for (const BlockRegistry::NonCubeElement& element : visual.elements) {
                    if (visual.shape == BlockRegistry::NonCubeShape::TransparentCube) {
                        MeshTransparentCube(element, block.nonCubeVisualId, chunk, world, x, y, z, blockOrigin,
                            textureManager, cutoutVertices, cutoutIndices);
                    } else if (visual.shape == BlockRegistry::NonCubeShape::Liquid) {
                        MeshLiquid(element, chunk, world, x, y, z, blockOrigin, textureManager,
                            translucentVertices, translucentIndices);
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
