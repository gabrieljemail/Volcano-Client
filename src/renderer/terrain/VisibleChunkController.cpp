#include "VisibleChunkController.hpp"
#include "models/Chunk.hpp"
#include <cmath>

namespace Volcano {

// Gribb/Hartmann plane extraction: each frustum plane is a row combination
// of the view-projection matrix. glm matrices are column-major and index as
// m[col][row], so row i of the matrix is (m[0][i], m[1][i], m[2][i], m[3][i]).
VisibleChunkController::Frustum VisibleChunkController::ExtractFrustum(const glm::mat4& viewProj)
{
    glm::vec4 row0(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]);
    glm::vec4 row1(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]);
    glm::vec4 row2(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]);
    glm::vec4 row3(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]);

    Frustum frustum;
    frustum.planes[0] = row3 + row0; // Left
    frustum.planes[1] = row3 - row0; // Right
    frustum.planes[2] = row3 + row1; // Bottom
    frustum.planes[3] = row3 - row1; // Top
    frustum.planes[4] = row3 + row2; // Near
    frustum.planes[5] = row3 - row2; // Far

    for (glm::vec4& plane : frustum.planes)
    {
        float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (length > 0.0f) plane /= length;
    }

    return frustum;
}

// "Positive vertex" AABB-vs-plane test: for each plane, pick the box corner
// furthest along the plane's inward normal and reject the box only if even
// that corner is outside — a box straddling a plane (the common case at
// frustum edges) survives, which is what a conservative cull needs.
bool VisibleChunkController::AabbIntersectsFrustum(const glm::vec3& min, const glm::vec3& max, const Frustum& frustum)
{
    for (const glm::vec4& plane : frustum.planes)
    {
        glm::vec3 positiveVertex(
            plane.x >= 0.0f ? max.x : min.x,
            plane.y >= 0.0f ? max.y : min.y,
            plane.z >= 0.0f ? max.z : min.z
        );

        if (glm::dot(glm::vec3(plane), positiveVertex) + plane.w < 0.0f) return false;
    }

    return true;
}

std::vector<const Mesh*> VisibleChunkController::GetVisibleMeshes(const std::vector<Mesh>& meshes, const glm::mat4& viewProj)
{
    Frustum frustum = ExtractFrustum(viewProj);

    std::vector<const Mesh*> visible;
    visible.reserve(meshes.size());

    for (const Mesh& mesh : meshes)
    {
        // Chunk meshes are only ever translated (never scaled/rotated), so
        // the translation column plus the fixed chunk extents is the AABB.
        glm::vec3 chunkMin(mesh.modelMatrix[3]);
        glm::vec3 chunkMax = chunkMin + glm::vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);

        if (AabbIntersectsFrustum(chunkMin, chunkMax, frustum)) visible.push_back(&mesh);
    }

    return visible;
}

} // namespace Volcano
