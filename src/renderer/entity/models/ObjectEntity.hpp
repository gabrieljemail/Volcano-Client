#pragma once
#ifndef OBJECT_ENTITY_MODEL_H
#define OBJECT_ENTITY_MODEL_H

#include <cstdint>
#include <glm/glm.hpp>
#include "Entity.hpp"

namespace Volcano {

// An inanimate, non-living entity: dropped items, boats, minecarts, thrown
// projectiles — what the protocol's Spawn Object packet covers, as opposed
// to a SubjectEntity (has its own AI/health) or PlayerEntity. Inherits
// Entity's common fields and adds the two that matter specifically for
// objects: unlike subjects/players, objects don't get frequent server
// position updates, so the client extrapolates motion from velocity between
// updates. Plain data, not polymorphic, so it still slices into a plain
// Entity for EntityRenderer's uniform batched draw.
struct ObjectEntity : public Entity {
    glm::vec3 velocity{0.0f};

    // Spawn Object's raw "object data" field — meaning depends on the
    // entity's type (e.g. block state id for a falling block, hanging
    // direction for item frames), so it's left opaque like Entity::type
    // until the runtime protocol registry can interpret it per-type.
    int32_t objectData = 0;
};

} // namespace Volcano

#endif
