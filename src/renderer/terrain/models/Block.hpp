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

    // Whether a non-cube block occupies enough of the voxel to collide with
    // (transparent cubes and partial shapes do; cross-shaped plants don't).
    // Meaningless when nonCubeVisualId is 0. See BlockRegistry::NonCubeVisual.
    bool nonCubeCollidable = false;

    bool isOpaque() const {
        return visualId != 0;
    }

    // Solid for collision purposes: every opaque full cube, plus any
    // non-cube shape flagged collidable (glass, slabs, ...) — but not
    // cross-shaped plants, which players walk straight through.
    bool hasCollision() const {
        return isOpaque() || (nonCubeVisualId != 0 && nonCubeCollidable);
    }
};

} // namespace Volcano

#endif
