#pragma once
#ifndef VOLCANO_USE_ITEM_EVENT_H
#define VOLCANO_USE_ITEM_EVENT_H

#include "../../GlobalState.hpp"
#include "../../network/NetworkClient.hpp"
#include "../InteractionTypes.hpp"

namespace Volcano {

// A secondary-trigger click with nothing targeted
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
