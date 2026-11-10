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
// and projection consistent with each other
struct OcclusionSnapshot {
    bool valid = false;
    // Bumped by HiZBuildPass::BeginFrame only when a published snapshot
    // actually differs from the previous one (a static view republishes
    // identical depth), so cached visibility results can tell when to expire.
    uint64_t serial = 0;
    glm::mat4 viewProj{1.0f};
    uint32_t fullWidth = 0, fullHeight = 0; // mip 0 (framebuffer) size
    uint32_t level = 0;                     // which mip `depth` holds
    uint32_t width = 0, height = 0;         // that mip's size
    std::vector<float> depth;               // width * height, row-major, max-reduced

    // Stats for the debug HUD / periodic [HIZ] log line, reset each frame.
    // Only counted while collectStats is set (the debug HUD is visible).
    bool collectStats = false;
    uint32_t tested = 0, occluded = 0;

    // True only when the whole AABB is provably behind already-drawn
    // geometry. Anything uncertain (no snapshot, straddling the near plane,
    // off-screen in the snapshot's view) returns false, i.e. stays visible.
    bool IsOccluded(const glm::vec3& min, const glm::vec3& max) const;

    // Same test for a moving thing: the box around `feet` (bottom center),
    // padded so the pyramid's lag doesn't clip an entity walking out from
    // behind cover.
    bool IsEntityOccluded(const glm::vec3& feet, float width, float height) const
    {
        constexpr float PAD = 0.3f;
        float half = width * 0.5f + PAD;
        return IsOccluded(feet + glm::vec3(-half, -PAD, -half), feet + glm::vec3(half, height + PAD, half));
    }
};

// Single instance, owned by the render thread
inline OcclusionSnapshot occlusionSnapshot;

} // namespace Volcano

#endif
