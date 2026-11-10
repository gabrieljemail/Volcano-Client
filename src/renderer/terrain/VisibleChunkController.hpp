#pragma once
#ifndef VOLCANO_VISIBLE_CHUNK_CONTROLLER_H
#define VOLCANO_VISIBLE_CHUNK_CONTROLLER_H

#include <array>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "../models/Mesh.hpp"
#include "models/ChunkSection.hpp"

namespace Volcano {

// Frustum culling for chunk section meshes. RenderThread hands this the
// current view-projection matrix and state->renderList (already lock-held
// at that point
class VisibleChunkController {
public:
    // Returns pointers into `meshes` for every mesh whose section AABB
    // intersects the frustum described by `viewProj` (camera view * proj,
    // the same matrices the render engine uploads into CameraUBO
    static std::vector<const Mesh*> GetVisibleMeshes(
        const std::unordered_map<SectionKey, Mesh, SectionKeyHash>& meshes, const glm::mat4& viewProj,
        bool useOcclusion = true);

private:
    // Plane equations ax+by+cz+d=0, stored as vec4(a,b,c,d) with the normal
    // (a,b,c) pointing inward, toward the visible side of the frustum.
    struct Frustum {
        std::array<glm::vec4, 6> planes;
    };

    static Frustum ExtractFrustum(const glm::mat4& viewProj);
    static bool AabbIntersectsFrustum(const glm::vec3& min, const glm::vec3& max, const Frustum& frustum);
};

} // namespace Volcano

#endif
