#pragma once
#ifndef VOLCANO_USE_ITEM_EVENT_H
#define VOLCANO_USE_ITEM_EVENT_H

#include "../../GlobalState.hpp"
#include "../../network/NetworkClient.hpp"
#include "../InteractionTypes.hpp"

namespace Volcano {

// A secondary-trigger click with nothing targeted - no block or entity in
// reach (see InteractionManager::SecondaryTriggerStart/Hold). Eating,
// drinking, drawing a bow, throwing an ender pearl, blocking with a shield,
// etc. all go through this same packet; what actually happens is entirely
// server-side based on what's in `hand`.
struct UseItemEvent {
    Hand hand;
    float yaw;
    float pitch;
    // Vanilla only swings for instant uses (equipping, throwing); eating,
    // blocking and drawing a bow don't.
    bool swing = true;

    void Dispatch(GlobalState* state) const
    {
        SendUseItem(state, hand, yaw, pitch);
        if (swing) SendArmAnimation(state, hand);
    }
};

} // namespace Volcano

#endif
