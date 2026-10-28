#pragma once
#ifndef VOLCANO_ITEM_USE_EVENT_H
#define VOLCANO_ITEM_USE_EVENT_H

#include <glm/glm.hpp>
#include "../../GlobalState.hpp"
#include "../../inventory/ItemStack.hpp"
#include "../InteractionTypes.hpp"

namespace Volcano {

// STUB. Meant to eventually replace InteractionManager::SecondaryTriggerStart's
// own inline block-place/interact-entity/use-item branching with a single
// per-item decision: given the held ItemStack and whatever's targeted, decide
// what actually happens (place a block, use a bucket on one, eat, draw a bow,
// swing a hoe, ...) the way vanilla's own Item::useOn/use methods do.
//
// That needs real per-item behavior data (is this a block item, is it edible,
// does it have a useOn-block effect, ...) that ItemRegistry doesn't parse
// yet — see SecondaryTriggerStart's own comment on the simplification this
// is standing in for. Until then, InteractionManager keeps making that call
// itself (whichever of targetedEntityId/targetedBlock/neither is set),
// dispatching straight to InteractEntityEvent/PlaceBlockEvent/UseItemEvent.
// This type isn't constructed or dispatched by anything yet.
struct ItemUseEvent {
    ItemStack item;
    glm::ivec3 position{};
    glm::ivec3 face{};
    bool hasBlockTarget = false;
    uint32_t entityId = 0;
    bool hasEntityTarget = false;
    Hand hand = Hand::Main;

    void Dispatch(GlobalState* /*state*/) const
    {
        // TODO: once ItemRegistry carries per-item behavior data, resolve it
        // here instead of in InteractionManager — see the class comment.
    }
};

} // namespace Volcano

#endif
