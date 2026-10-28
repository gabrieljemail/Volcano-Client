#pragma once
#ifndef VOLCANO_GOLEM_MODEL_H
#define VOLCANO_GOLEM_MODEL_H

#include <vector>
#include "EntityBoxMesh.hpp"

namespace Volcano::GolemModel {

// Six-box static mesh (head, body, 2 arms, 2 legs) for the iron golem - its
// own proportions (a big flat-topped body, arms almost as long as the body
// is tall, short thick legs) rather than either the flat cube or
// HumanoidModel's much slimmer player/zombie skeleton. Same normalized
// -0.5..0.5 space and batched/instanced draw path as QuadrupedModel - see
// its own comment for why (entity.vert rescales this per-instance against
// EntityRegistry's iron_golem width/height entry).
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
