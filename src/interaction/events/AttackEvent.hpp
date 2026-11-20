#pragma once
#ifndef VOLCANO_ATTACK_EVENT_H
#define VOLCANO_ATTACK_EVENT_H

#include <cstdint>
#include "../../ApplicationDeps.hpp"
#ifdef VOLCANO_MODULES
import Volcano;
#else
#include "../../Application.hpp"
#endif
#include "../../network/NetworkClient.hpp"

namespace Volcano {

// A primary-trigger click that landed on an entity (see
// InteractionManager::PrimaryTrigger) - melee-attacking it with whatever's
// in the mainhand slot. Vanilla always attacks with the mainhand; there's
// no such thing as an offhand attack.
struct AttackEvent {
    uint32_t entityId;

    void Dispatch(Application* state) const
    {
        SendAttack(state, entityId);
        SendArmAnimation(state, Hand::Main);
    }
};

} // namespace Volcano

#endif
