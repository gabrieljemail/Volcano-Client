#pragma once
#ifndef VOLCANO_BLOCK_ENTITY_INTERACT_EVENT_H
#define VOLCANO_BLOCK_ENTITY_INTERACT_EVENT_H

#include <glm/glm.hpp>
#include "../../ApplicationDeps.hpp"
#ifdef VOLCANO_MODULES
import Volcano;
#else
#include "../../Application.hpp"
#endif
#include "../../network/NetworkClient.hpp"
#include "../InteractionTypes.hpp"

namespace Volcano {

// A secondary-trigger click on a block entity (chest, furnace, sign, ...)
// while not sneaking
struct BlockEntityInteractEvent {
    glm::ivec3 position;
    glm::ivec3 face;
    Hand hand;

    void Dispatch(Application* state) const
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
