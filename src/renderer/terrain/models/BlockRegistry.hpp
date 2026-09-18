#pragma once
#ifndef VOLCANO_BLOCK_REGISTRY_H
#define VOLCANO_BLOCK_REGISTRY_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "Block.hpp"

namespace Volcano::BlockRegistry {

// Parses resources/minecraft-data's blocks.json (protocol block-state ids
// per block name) together with the vanilla resource pack's blockstate/model
// JSON (resources/minecraft/assets/minecraft/{blockstates,models}) to build
// a protocol state-id -> face-texture-name table. Must be called once at
// startup, before any chunk data is parsed or meshed. Not thread-safe with
// itself; MapStateId/GetFaceTextureName/VisualIdForName are read-only after
// Init() returns and safe to call from any thread.
void Init();

// Maps a protocol block-state id (as sent in a Chunk Data and Update Light
// packet's paletted containers) to a visual id — an index into an interned
// table of face-texture-name tuples. 0 means "nothing to draw here": true
// air, a block Init() couldn't resolve to a full single-cube model (stairs,
// slabs, fences, plants, fluids, ...), or a state id outside every loaded
// block's range (logged once per id — a real version/protocol mismatch
// signal, unlike the other two cases which are expected).
uint16_t MapStateId(int32_t stateId);

// Resource-pack texture name (a bare stem, e.g. "stone", "grass_block_top")
// for one face of a visual id, in ChunkMesher's face convention: 0=Up,
// 1=Down, 2=North, 3=South, 4=East, 5=West. visualId 0 (or any id with no
// resolved texture for the requested face) returns a generic fallback name.
const std::string& GetFaceTextureName(uint16_t visualId, int faceIndex);

// Looks up the visual id resolved for a block by name (e.g. "stone",
// "oak_log"). Returns 0 (nothing to draw) if the name is unknown or wasn't
// resolvable to a full-cube visual. Used by callers that want a specific
// block's appearance without going through a protocol state id (e.g. the
// offline debug world generator).
uint16_t VisualIdForName(const std::string& blockName);

// How a NonCubeVisual should be drawn by NonCubicMesher. (Collision no longer
// comes from these visuals — see MapStateIdCollisionShape.)
enum class NonCubeShape : uint8_t {
    Cross,           // Two (or more) intersecting billboard planes: grass, flowers, saplings, ...
    Partial,         // One or more axis-aligned boxes smaller than a full block: slabs, carpets, snow layers, ...
    TransparentCube, // A full unit cube, but not opaque: glass, slime, ice, ...
};

// One axis-aligned box within a Partial/TransparentCube visual, in the same
// model-space units as the resource-pack JSON (0..16 per axis, 1 unit =
// 1/16 block) — NonCubicMesher divides by 16 itself. An empty faceTextures
// entry means that face isn't part of the model (nothing drawn there),
// same convention BlockRegistry::GetFaceTextureName uses for full cubes.
struct NonCubeElement {
    glm::vec3 from{0.0f};
    glm::vec3 to{0.0f};
    std::array<std::string, 6> faceTextures; // ChunkMesher's face order: 0=Up, 1=Down, 2=North, 3=South, 4=East, 5=West.
    // Per-face texture-space rectangle (u1,v1,u2,v2), same 0..16 units as
    // from/to — the model JSON's own "uv", or a full 0,0,16,16 tile when a
    // face doesn't specify one. Needed because a sub-full-block element
    // (a torch's 2x10x2 stick, a slab's 16x8x16 half) samples only part of
    // its texture, not the whole thing stretched across the smaller face —
    // see NonCubicMesher's EmitQuad.
    std::array<glm::vec4, 6> faceUVs;
};

struct NonCubeVisual {
    NonCubeShape shape = NonCubeShape::Partial;
    std::vector<NonCubeElement> elements; // Populated for Partial/TransparentCube; empty for Cross.
    std::string crossTexture;             // Populated for Cross only.
};

// Maps a protocol block-state id to a non-cube visual id — an index into an
// interned table of NonCubeVisuals — for any block that didn't resolve to an
// opaque full-cube visual via MapStateId. 0 means nothing to draw this way
// either (true air, an out-of-range id, or a shape this pass still doesn't
// handle — fluids and multipart blockstates like fences/walls/redstone).
uint16_t MapStateIdNonCube(int32_t stateId);

// Looks up a previously-resolved non-cube visual by id. Returns a
// default-constructed (Partial, no elements) NonCubeVisual
// for id 0 or any id outside the interned table.
const NonCubeVisual& GetNonCubeVisual(uint16_t nonCubeVisualId);

// One axis-aligned collision box in LOCAL block-space, 0..1 per axis — add
// directly to a block's integer world origin to place it. May extend past 1
// on Y: fences, walls and fence gates collide up to 1.5 (see TickLoop's scan
// ranges, which look one cell below the player's AABB for exactly this).
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

// Small fixed-capacity box list, returned by value with no heap allocation,
// since this runs on TickLoop's physics hot path every tick for every block
// cell a player's AABB might touch. Sized for the largest shape in 26.1's
// blockCollisionShapes.json (15 boxes); anything beyond MAX_BOXES is dropped
// with a one-time log at load.
struct CollisionBoxes {
    static constexpr int MAX_BOXES = 16;
    std::array<AABB, MAX_BOXES> boxes{};
    int count = 0;
};

// Maps a protocol block-state id to its collision-shape id in minecraft-data's
// blockCollisionShapes.json — the same per-state shapes the server collides
// against. 0 means no collision. Returns Block::UNKNOWN_COLLISION_SHAPE for
// an out-of-range id, or if that file failed to load.
uint16_t MapStateIdCollisionShape(int32_t stateId);

// Real collision geometry for one block, from its collisionShapeId — NOT from
// its render model. Deriving it from the model (as this used to) left every
// block without drawable model elements with no collision: shulker boxes,
// chests, beds, signs, skulls, and anything else a block-entity renderer
// draws, plus fences/walls (multipart blockstates this registry doesn't
// resolve visuals for). The server still treats all of those as solid, so
// standing on one meant sinking into it client-side and being teleported
// back every tick. Blocks with UNKNOWN_COLLISION_SHAPE (the offline debug
// world, or collision data that failed to load) fall back to "opaque full
// cube or nothing". Shared by TickLoop's physics collision and (later)
// block-selection raycasting so both agree on the exact same shape.
CollisionBoxes GetCollisionBoxes(const Block& block);

} // namespace Volcano::BlockRegistry

#endif
