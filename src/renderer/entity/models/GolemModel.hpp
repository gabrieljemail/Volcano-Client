#pragma once
#ifndef VOLCANO_GOLEM_MODEL_H
#define VOLCANO_GOLEM_MODEL_H

#include <vector>
#include "EntityBoxMesh.hpp"

namespace Volcano::GolemModel {

// Six-box static mesh (head, body, 2 arms, 2 legs) for the iron golem
inline void BuildMesh(std::vector<EntityMeshVertex>& vertices, std::vector<uint32_t>& indices)
{
    AppendBox(vertices, indices, {  0.00f,  0.42f,  0.15f }, { 0.30f, 0.16f, 0.30f }); // head
    AppendBox(vertices, indices, {  0.00f,  0.05f,  0.00f }, { 0.55f, 0.55f, 0.35f }); // body
    AppendBox(vertices, indices, {  0.40f, -0.05f,  0.00f }, { 0.16f, 0.65f, 0.22f }); // right arm
    AppendBox(vertices, indices, { -0.40f, -0.05f,  0.00f }, { 0.16f, 0.65f, 0.22f }); // left arm
    AppendBox(vertices, indices, {  0.16f, -0.35f,  0.00f }, { 0.22f, 0.30f, 0.22f }); // right leg
    AppendBox(vertices, indices, { -0.16f, -0.35f,  0.00f }, { 0.22f, 0.30f, 0.22f }); // left leg
}

} // namespace Volcano::GolemModel

#endif
