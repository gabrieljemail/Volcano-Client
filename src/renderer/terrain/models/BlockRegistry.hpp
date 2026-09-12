#pragma once
#ifndef VOLCANO_BLOCK_REGISTRY_H
#define VOLCANO_BLOCK_REGISTRY_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

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

// How a NonCubeVisual should be drawn/collided with by NonCubicMesher and
// Block::hasCollision() respectively.
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
};

struct NonCubeVisual {
    NonCubeShape shape = NonCubeShape::Partial;
    bool collidable = false;
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
// default-constructed (Partial, not collidable, no elements) NonCubeVisual
// for id 0 or any id outside the interned table.
const NonCubeVisual& GetNonCubeVisual(uint16_t nonCubeVisualId);

} // namespace Volcano::BlockRegistry

#endif
