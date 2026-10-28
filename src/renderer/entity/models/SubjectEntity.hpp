#pragma once
#ifndef SUBJECT_ENTITY_MODEL_H
#define SUBJECT_ENTITY_MODEL_H

#include "Entity.hpp"

namespace Volcano {

// A server-controlled living entity: mobs, animals, villagers
struct SubjectEntity : public Entity {
    float health = 1.0f;
    float maxHealth = 1.0f;

    // Baby variants of most mobs render smaller and move differently -
    // kept as a flag rather than folded into a size scalar since it's what
    // the protocol actually sends (a metadata bit), not a derived value.
    bool baby = false;
};

} // namespace Volcano

#endif
