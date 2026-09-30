#pragma once
#ifndef VOLCANO_OCCLUSION_SNAPSHOT_H
#define VOLCANO_OCCLUSION_SNAPSHOT_H

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace Volcano {

// A CPU copy of one small mip of the Hi-Z pyramid (see HiZBuildPass), plus
// the view-projection it was rendered with. HiZBuildPass refreshes it at the
// start of every frame from a readback that's a couple of frames old (the
// accepted occlusion lag), and VisibleChunkController tests chunk-section
// AABBs against it.
//
// Testing with the snapshot's OWN viewProj (not this frame's) keeps depth
// and projection consistent with each other — no reprojection needed. The
// cost is that something newly revealed pops in a frame or two late.
//
// Render-thread only: written in SubmitFrame before any pass records, and
// read by TerrainPass/NonCubicPass during that same frame.
struct OcclusionSnapshot {
    bool valid = false;
    glm::mat4 viewProj{1.0f};
    uint32_t fullWidth = 0, fullHeight = 0; // mip 0 (framebuffer) size
    uint32_t level = 0;                     // which mip `depth` holds
    uint32_t width = 0, height = 0;         // that mip's size
    std::vector<float> depth;               // width * height, row-major, max-reduced

    // Stats for the periodic [HIZ] log line, reset each frame.
    uint32_t tested = 0, occluded = 0;

    // True only when the whole AABB is provably behind already-drawn
    // geometry. Anything uncertain (no snapshot, straddling the near plane,
    // off-screen in the snapshot's view) returns false, i.e. stays visible.
    bool IsOccluded(const glm::vec3& min, const glm::vec3& max) const;
};

// Single instance, owned by the render thread — see the comment above.
inline OcclusionSnapshot occlusionSnapshot;

} // namespace Volcano

#endif
