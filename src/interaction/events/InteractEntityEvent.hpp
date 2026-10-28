#pragma once
#ifndef VOLCANO_INTERACT_ENTITY_EVENT_H
#define VOLCANO_INTERACT_ENTITY_EVENT_H

#include <cstdint>
#include "../../GlobalState.hpp"
#include "../../network/NetworkClient.hpp"
#include "../InteractionTypes.hpp"

namespace Volcano {

// A secondary-trigger click that landed on an entity (see
// InteractionManager::SecondaryTriggerStart) - right-clicking it (feeding
// an animal, opening a villager's trades, mounting a horse, ...).
struct InteractEntityEvent {
    uint32_t entityId;
    Hand hand;
    bool sneaking;

    void Dispatch(GlobalState* state) const
    {
        SendInteractEntity(state, entityId, hand, sneaking);
        SendArmAnimation(state, hand);
    }
};

} // namespace Volcano

#endif
