#pragma once
#ifndef BLOCK_H
#define BLOCK_H

#include <cstdint>

namespace Volcano {

struct Block {
    // Index into BlockRegistry's interned BlockVisual table. 0 is reserved
    // for both true air and any block BlockRegistry couldn't (yet) resolve
    // to a full-cube visual (non-cube shapes, fluids, multipart blockstates,
    // etc.) — those either render as nothing or go through nonCubeVisualId
    // below instead.
    uint16_t visualId = 0;

    // Index into BlockRegistry's interned NonCubeVisual table (see
    // BlockRegistry.hpp) — set only when visualId is 0 and the block
    // resolved to a transparent full cube (glass, slime, ...), a
    // partial-volume shape (slabs, carpets, ...), or a cross-shaped plant
    // (grass, flowers, ...). 0 means nothing to draw here via that path
    // either. Meshed separately by NonCubicMesher, not ChunkMesher.
    uint16_t nonCubeVisualId = 0;

    // Sentinel for collisionShapeId: this Block wasn't built from a protocol
    // state id (e.g. the offline debug world's Chunk::setBlock(visualId)
    // overload), or the collision data failed to load. GetCollisionBoxes
    // falls back to "opaque full cube or nothing" for these.
    static constexpr uint16_t UNKNOWN_COLLISION_SHAPE = 0xFFFF;

    // Index into BlockRegistry's collision-shape table (minecraft-data's
    // blockCollisionShapes.json), resolved per protocol state id — 0 means no
    // collision at all (air, plants, fluids). Deliberately independent of
    // the two visual ids above: collision used to be derived from the render
    // model, which gave every block without drawable model geometry (shulker
    // boxes, chests, beds, signs, ... — anything drawn by a block-entity
    // renderer) no collision at all, so the client let the player sink
    // into blocks the server treats as solid and got teleported back for it.
    uint16_t collisionShapeId = UNKNOWN_COLLISION_SHAPE;

    bool isOpaque() const {
        return visualId != 0;
    }

    // Solid for collision purposes, per the real collision shape — see
    // collisionShapeId.
    bool hasCollision() const {
        return collisionShapeId == UNKNOWN_COLLISION_SHAPE ? isOpaque() : collisionShapeId != 0;
    }
};

} // namespace Volcano

#endif
