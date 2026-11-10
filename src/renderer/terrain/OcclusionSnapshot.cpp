#include "OcclusionSnapshot.hpp"
#include <algorithm>
#include <cmath>

namespace Volcano {

bool OcclusionSnapshot::IsOccluded(const glm::vec3& min, const glm::vec3& max) const
{
    if (!valid || width == 0 || height == 0) return false;

    float ndcMinX = 1e30f, ndcMinY = 1e30f, ndcMaxX = -1e30f, ndcMaxY = -1e30f;
    float nearestDepth = 1e30f;

    for (int i = 0; i < 8; i++)
    {
        glm::vec4 corner((i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z, 1.0f);
        glm::vec4 clip = viewProj * corner;
        // Behind (or right at) the camera: the projected rectangle is
        // meaningless, and a box this close is almost certainly visible.
        if (clip.w <= 1e-4f) return false;

        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        ndcMinX = std::min(ndcMinX, ndc.x); ndcMaxX = std::max(ndcMaxX, ndc.x);
        ndcMinY = std::min(ndcMinY, ndc.y); ndcMaxY = std::max(ndcMaxY, ndc.y);
        // clip.z / clip.w is exactly what the rasterizer writes to the
        // depth buffer, so it compares directly against the pyramid.
        nearestDepth = std::min(nearestDepth, ndc.z);
    }

    if (nearestDepth <= 0.0f) return false; // crosses the near plane
    // Entirely off-screen in the snapshot's view: nothing to compare
    // against. Frustum culling already handles this frame's view.
    if (ndcMaxX < -1.0f || ndcMinX > 1.0f || ndcMaxY < -1.0f || ndcMinY > 1.0f) return false;

    // NDC -> mip 0 pixels. The projection's Y is pre-flipped (p[1][1] *= -1
    // in RenderThread), so NDC -1 is already the framebuffer's top row.
    auto toPixel = [](float ndc, uint32_t size) {
        return std::clamp((ndc * 0.5f + 0.5f) * static_cast<float>(size), 0.0f, static_cast<float>(size - 1));
    };
    float px0 = toPixel(ndcMinX, fullWidth), px1 = toPixel(ndcMaxX, fullWidth);
    float py0 = toPixel(ndcMinY, fullHeight), py1 = toPixel(ndcMaxY, fullHeight);

    // Mip `level` texel t covers mip 0 pixels [t << level, (t+1) << level);
    // the last texel on each edge also absorbs any odd remainder (see
    // hiz_downsample.comp), hence the clamp to width/height - 1.
    const float scale = static_cast<float>(1u << level);
    uint32_t tx0 = std::min(static_cast<uint32_t>(px0 / scale), width - 1);
    uint32_t tx1 = std::min(static_cast<uint32_t>(px1 / scale), width - 1);
    uint32_t ty0 = std::min(static_cast<uint32_t>(py0 / scale), height - 1);
    uint32_t ty1 = std::min(static_cast<uint32_t>(py1 / scale), height - 1);

    for (uint32_t y = ty0; y <= ty1; y++)
    {
        const float* row = &depth[static_cast<size_t>(y) * width];
        for (uint32_t x = tx0; x <= tx1; x++)
        {
            // Some part of the rectangle has drawn geometry at or beyond
            // the box's nearest point (or sky, depth 1.0)
            if (row[x] >= nearestDepth) return false;
        }
    }

    return true;
}

} // namespace Volcano
