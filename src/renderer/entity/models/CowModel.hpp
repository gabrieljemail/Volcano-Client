#pragma once
#ifndef VOLCANO_COW_MODEL_H
#define VOLCANO_COW_MODEL_H

#include <vector>
#include "EntityBoxMesh.hpp"

namespace Volcano::CowModel {

// Six-box static mesh (head, body, 4 legs), proportions converted from real
// vanilla CowModel.java (legSize=12, real cow hitbox 0.9 wide x 1.4 tall) -
// horns/udder flap omitted, same 6-box budget as QuadrupedModel.hpp/
// PigModel.hpp. Same -0.5..0.5 normalized/EntityRegistry-rescaled
// convention and Z negation as PigModel - see its own comment.
inline void BuildMesh(std::vector<EntityMeshVertex>& vertices, std::vector<uint32_t>& indices)
{
    AppendBox(vertices, indices, { 0.00f,  0.26f, -0.07f }, { 0.83f, 0.45f, 1.25f }); // body
    AppendBox(vertices, indices, { 0.00f,  0.39f,  0.76f }, { 0.56f, 0.36f, 0.42f }); // head
    AppendBox(vertices, indices, {-0.28f, -0.23f,  0.35f }, { 0.28f, 0.54f, 0.28f }); // front-right leg
    AppendBox(vertices, indices, { 0.28f, -0.23f,  0.35f }, { 0.28f, 0.54f, 0.28f }); // front-left leg
    AppendBox(vertices, indices, {-0.28f, -0.23f, -0.49f }, { 0.28f, 0.54f, 0.28f }); // back-right leg
    AppendBox(vertices, indices, { 0.28f, -0.23f, -0.49f }, { 0.28f, 0.54f, 0.28f }); // back-left leg
}

} // namespace Volcano::CowModel

#endif
