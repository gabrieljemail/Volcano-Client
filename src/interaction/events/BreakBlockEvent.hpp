#pragma once
#ifndef VOLCANO_BREAK_BLOCK_EVENT_H
#define VOLCANO_BREAK_BLOCK_EVENT_H

#include <mutex>
#include <vector>
#include <glm/glm.hpp>
#include "../../GlobalState.hpp"
#include "../../network/NetworkClient.hpp"
#include "../../renderer/terrain/models/ChunkSection.hpp"
#include "../InteractionTypes.hpp"

namespace Volcano {

// Completion of a block break — either instant (canInstabuild, or a
// hardness-0 block) or the end of a real timed hold (see
// InteractionManager::BeginMining/ContinueMining, which own the actual
// per-block timing via BlockRegistry::MapStateIdBreakTicks).
//
// Applies the edit locally (World::SetBlock, to air) for immediate visual
// feedback and enqueues the affected section(s) for remeshing — local-only
// prediction, reconciled against the server by the clientbound Block Update
// handler (NetworkClient.cpp's PlayS2C::BlockUpdate, via NetworkInbox::
// pendingBlockEdits) if the server rejects it.
struct BreakBlockEvent {
    glm::ivec3 position;
    glm::ivec3 face;

    // False when BeginMining already sent StartDigging itself, earlier, at
    // the start of a real timed hold — resending it right before
    // FinishDigging would look like an instant break on the wire.
    bool sendStart = true;

    void Dispatch(GlobalState* state) const
    {
        if (sendStart) SendPlayerAction(state, DigStatus::StartDigging, position, face);
        SendPlayerAction(state, DigStatus::FinishDigging, position, face);
        SendArmAnimation(state, Hand::Main);

        std::vector<SectionKey> dirty = state->world->SetBlock(position, static_cast<uint16_t>(0)); // 0 = air
        if (!dirty.empty())
        {
            std::lock_guard<std::mutex> lock(state->networkInbox.mutex);
            for (const SectionKey& key : dirty) state->networkInbox.dirtySections.push_back(key);
        }
    }
};

} // namespace Volcano

#endif
