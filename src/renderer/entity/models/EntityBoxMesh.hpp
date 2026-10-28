#pragma once
#ifndef VOLCANO_ENTITY_BOX_MESH_H
#define VOLCANO_ENTITY_BOX_MESH_H

#include <array>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace Volcano {

// Shared vertex format for every untextured procedural entity mesh (the
// placeholder cube, QuadrupedModel, GolemModel) - flat-normal, no UV, tinted
// per-instance by EntityRenderer's GPUEntityInstance color/hurt fields
// instead of sampling a texture. Must stay layout-compatible with
// EntityRenderer.cpp's own EntityVertex alias (same two vec3 fields, same
// order) - that's the type actually bound to the pipeline's vertex input
// state.
struct EntityMeshVertex
{
    glm::vec3 position;
    glm::vec3 normal;
};

// Appends one axis-aligned box (given its center and full size) to
// vertices/indices - 4 unique vertices per face (24 total) so each face
// keeps its own flat normal. Generalizes what used to be
// EntityRenderer::BuildCubeMesh's single hard-coded unit box; every
// procedural entity mesh should build its boxes through this rather than
// re-deriving the winding by hand. Winding: per face, u x v == normal,
// corners taken in (-u,-v)->(+u,-v)->(+u,+v)->(-u,+v) order
inline void AppendBox(std::vector<EntityMeshVertex>& vertices, std::vector<uint32_t>& indices,
    const glm::vec3& center, const glm::vec3& size)
{
    struct Face { glm::vec3 normal, u, v; };
    constexpr std::array<Face, 6> faces = {{
        { glm::vec3( 1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1) },
        { glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0) },
        { glm::vec3( 0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0) },
        { glm::vec3( 0,-1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1) },
        { glm::vec3( 0, 0, 1), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0) },
        { glm::vec3( 0, 0,-1), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0) },
    }};

    for (const Face& face : faces)
    {
        glm::vec3 faceCenter = center + face.normal * (glm::dot(size, glm::abs(face.normal)) * 0.5f);
        float halfU = glm::dot(size, glm::abs(face.u)) * 0.5f;
        float halfV = glm::dot(size, glm::abs(face.v)) * 0.5f;

        uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.push_back({ faceCenter - halfU * face.u - halfV * face.v, face.normal });
        vertices.push_back({ faceCenter + halfU * face.u - halfV * face.v, face.normal });
        vertices.push_back({ faceCenter + halfU * face.u + halfV * face.v, face.normal });
        vertices.push_back({ faceCenter - halfU * face.u + halfV * face.v, face.normal });

        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
}

} // namespace Volcano

#endif
