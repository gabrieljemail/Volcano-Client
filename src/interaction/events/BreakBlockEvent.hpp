#pragma once
#ifndef VOLCANO_BREAK_BLOCK_EVENT_H
#define VOLCANO_BREAK_BLOCK_EVENT_H

#include <mutex>
#include <vector>
#include <glm/glm.hpp>
#include "../../GlobalState.hpp"
#include "../../audio/BlockSounds.hpp"
#include "../../renderer/terrain/LightEngine.hpp"
#include "../../network/NetworkClient.hpp"
#include "../../renderer/terrain/models/ChunkSection.hpp"
#include "../InteractionTypes.hpp"

namespace Volcano {

// Completion of a block break
struct BreakBlockEvent {
    glm::ivec3 position;
    glm::ivec3 face;

    // False when BeginMining already sent StartDigging itself, earlier, at
    // the start of a real timed hold
    bool sendStart = true;

    void Dispatch(GlobalState* state) const
    {
        if (sendStart) SendPlayerAction(state, DigStatus::StartDigging, position, face);
        SendPlayerAction(state, DigStatus::FinishDigging, position, face);
        SendArmAnimation(state, Hand::Main);

        Block oldBlock = state->world->GetBlock(position.x, position.y, position.z);
        if (state->blockSounds != nullptr) state->blockSounds->PlayBreak(oldBlock);

        std::vector<SectionKey> dirty = state->world->SetBlock(position, static_cast<uint16_t>(0)); // 0 = air
        if (state->lightEngine != nullptr)
        {
            for (const SectionKey& key : state->lightEngine->OnBlockChanged(position, oldBlock, Block{})) dirty.push_back(key);
        }
        if (!dirty.empty())
        {
            std::lock_guard<std::mutex> lock(state->networkInbox.mutex);
            for (const SectionKey& key : dirty) state->networkInbox.dirtySections.push_back(key);
        }
    }
};

} // namespace Volcano

#endif
