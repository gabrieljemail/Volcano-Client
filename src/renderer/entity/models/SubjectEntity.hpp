#pragma once
#ifndef SUBJECT_ENTITY_MODEL_H
#define SUBJECT_ENTITY_MODEL_H

#include "Entity.hpp"

namespace Volcano {

// A server-controlled living entity: mobs, animals, villagers — anything
// with its own health/AI that isn't a player (see PlayerEntity.hpp) or an
// inanimate object (see ObjectEntity.hpp). Inherits Entity's common fields
// and adds the ones every living thing has regardless of species, which the
// runtime protocol registry hasn't classified yet. Plain data, not
// polymorphic, so it still slices into a plain Entity for EntityRenderer's
// uniform batched draw.
struct SubjectEntity : public Entity {
    float health = 1.0f;
    float maxHealth = 1.0f;

    // Baby variants of most mobs render smaller and move differently —
    // kept as a flag rather than folded into a size scalar since it's what
    // the protocol actually sends (a metadata bit), not a derived value.
    bool baby = false;
};

} // namespace Volcano

#endif
