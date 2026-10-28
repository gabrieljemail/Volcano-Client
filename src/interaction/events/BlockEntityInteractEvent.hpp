#pragma once
#ifndef VOLCANO_BLOCK_ENTITY_INTERACT_EVENT_H
#define VOLCANO_BLOCK_ENTITY_INTERACT_EVENT_H

#include <glm/glm.hpp>
#include "../../GlobalState.hpp"
#include "../../network/NetworkClient.hpp"
#include "../InteractionTypes.hpp"

namespace Volcano {

// A secondary-trigger click on a block entity (chest, furnace, sign, ...)
// while not sneaking - see InteractionManager::SecondaryTriggerStart.
//
// Vanilla uses the same use-item-on packet as placing; the server decides
// whether to open/toggle the block and responds with the appropriate window.
// Unlike PlaceBlockEvent, nothing is predicted locally.
struct BlockEntityInteractEvent {
    glm::ivec3 position;
    glm::ivec3 face;
    Hand hand;

    void Dispatch(GlobalState* state) const
    {
        glm::vec3 cursor(0.5f, 0.5f, 0.5f);
        if (face.x != 0) cursor.x = face.x > 0 ? 1.0f : 0.0f;
        if (face.y != 0) cursor.y = face.y > 0 ? 1.0f : 0.0f;
        if (face.z != 0) cursor.z = face.z > 0 ? 1.0f : 0.0f;

        SendBlockPlace(state, hand, position, face, cursor);
        SendArmAnimation(state, hand);
    }
};

} // namespace Volcano

#endif
