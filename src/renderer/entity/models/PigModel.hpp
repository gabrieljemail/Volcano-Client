#pragma once
#ifndef VOLCANO_PIG_MODEL_H
#define VOLCANO_PIG_MODEL_H

#include <vector>
#include "EntityBoxMesh.hpp"

namespace Volcano::PigModel {

// Six-box static mesh (head, body, 4 legs), proportions converted from real
// vanilla PigModel.java/QuadrupedModel.java (legSize=6, real pig hitbox
// 0.9x0.9) - not the generic QuadrupedModel.hpp shape. Same -0.5..0.5
// normalized/EntityRegistry-rescaled convention as QuadrupedModel
inline void BuildMesh(std::vector<EntityMeshVertex>& vertices, std::vector<uint32_t>& indices)
{
    AppendBox(vertices, indices, { 0.00f,  0.19f,  0.14f }, { 0.69f, 0.56f, 0.83f }); // body
    AppendBox(vertices, indices, { 0.00f,  0.33f,  0.69f }, { 0.56f, 0.56f, 0.56f }); // head
    AppendBox(vertices, indices, {-0.21f, -0.29f,  0.35f }, { 0.28f, 0.42f, 0.28f }); // front-right leg
    AppendBox(vertices, indices, { 0.21f, -0.29f,  0.35f }, { 0.28f, 0.42f, 0.28f }); // front-left leg
    AppendBox(vertices, indices, {-0.21f, -0.29f, -0.49f }, { 0.28f, 0.42f, 0.28f }); // back-right leg
    AppendBox(vertices, indices, { 0.21f, -0.29f, -0.49f }, { 0.28f, 0.42f, 0.28f }); // back-left leg
}

} // namespace Volcano::PigModel

#endif
