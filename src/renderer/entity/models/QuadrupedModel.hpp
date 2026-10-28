#pragma once
#ifndef VOLCANO_QUADRUPED_MODEL_H
#define VOLCANO_QUADRUPED_MODEL_H

#include <vector>
#include "EntityBoxMesh.hpp"

namespace Volcano::QuadrupedModel {

// Six-box static mesh (head, body, 4 legs) for four-legged mobs
inline void BuildMesh(std::vector<EntityMeshVertex>& vertices, std::vector<uint32_t>& indices)
{
    // Local +Z is forward, matching entity.vert's yaw rotation (identity at
    // yaw 0) and TickLoop's own forward vector - no coordinate flip needed
    // here, unlike HumanoidModel which ports vanilla's own -Z-forward space.
    AppendBox(vertices, indices, {  0.00f,  0.30f,  0.38f }, { 0.35f, 0.28f, 0.26f }); // head
    AppendBox(vertices, indices, {  0.00f,  0.05f, -0.05f }, { 0.60f, 0.42f, 0.75f }); // body
    AppendBox(vertices, indices, {  0.28f, -0.32f,  0.30f }, { 0.20f, 0.36f, 0.20f }); // front-right leg
    AppendBox(vertices, indices, { -0.28f, -0.32f,  0.30f }, { 0.20f, 0.36f, 0.20f }); // front-left leg
    AppendBox(vertices, indices, {  0.28f, -0.32f, -0.30f }, { 0.20f, 0.36f, 0.20f }); // back-right leg
    AppendBox(vertices, indices, { -0.28f, -0.32f, -0.30f }, { 0.20f, 0.36f, 0.20f }); // back-left leg
}

} // namespace Volcano::QuadrupedModel

#endif
