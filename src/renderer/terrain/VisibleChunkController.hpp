#pragma once
#ifndef VOLCANO_VISIBLE_CHUNK_CONTROLLER_H
#define VOLCANO_VISIBLE_CHUNK_CONTROLLER_H

#include <array>
#include <vector>
#include <glm/glm.hpp>
#include "../models/Mesh.hpp"

namespace Volcano {

// Frustum culling for chunk meshes. RenderThread hands this the current
// view-projection matrix and state->renderList (already lock-held at that
// point — see its call site) and gets back pointers to just the meshes
// worth binding/drawing this frame.
//
// Each Mesh's AABB is derived from its modelMatrix translation column (the
// chunk mesher places each chunk's world origin there, see
// ChunkMesher::MeshChunk) plus the fixed CHUNK_SIZE_X/Y/Z extents — chunk
// meshes are never scaled or rotated, so the rest of modelMatrix never
// matters here.
class VisibleChunkController {
public:
    // Returns pointers into `meshes` for every mesh whose chunk-column AABB
    // intersects the frustum described by `viewProj` (camera view * proj,
    // the same matrices RecordAndSubmitFrame uploads into CameraUBO).
    // `meshes` must outlive the returned vector.
    static std::vector<const Mesh*> GetVisibleMeshes(const std::vector<Mesh>& meshes, const glm::mat4& viewProj);

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
