#pragma once
#ifndef OBJECT_ENTITY_MODEL_H
#define OBJECT_ENTITY_MODEL_H

#include <cstdint>
#include <glm/glm.hpp>
#include "Entity.hpp"

namespace Volcano {

// An inanimate, non-living entity: dropped items, boats, minecarts, thrown
// projectiles
struct ObjectEntity : public Entity {
    glm::vec3 velocity{0.0f};

    // Spawn Object's raw "object data" field
    int32_t objectData = 0;
};

} // namespace Volcano

#endif
