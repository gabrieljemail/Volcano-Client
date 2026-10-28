#pragma once
#ifndef VOLCANO_PLACE_BLOCK_EVENT_H
#define VOLCANO_PLACE_BLOCK_EVENT_H

#include <mutex>
#include <vector>
#include <glm/glm.hpp>
#include "../../GlobalState.hpp"
#include "../../audio/BlockSounds.hpp"
#include "../../renderer/terrain/LightEngine.hpp"
#include "../../network/NetworkClient.hpp"
#include "../../renderer/terrain/models/BlockRegistry.hpp"
#include "../InteractionTypes.hpp"

namespace Volcano {

// A secondary-trigger click that landed on a block while holding something
// placeable (see InteractionManager::SecondaryTriggerStart) - placing a
// block, or using a bucket/bonemeal/dye/etc. against one.
//
// `position`/`face` are the EXISTING block being clicked and which of its
// faces (straight off TargetedBlock - see RaycastBlocks), not the new cell
// the placed block will occupy; the server derives that from position+face
// itself.
//
// If `predictedStateId` is set (the held item is a block), the new block is
// also placed locally right away. It's the block's default state, so
// orientation (log axis, stair facing, ...) can be wrong until the server's
// Block Update arrives. Vanilla sends that update for the target cell
// whether the place succeeded or not, so a rejected place gets undone too.
struct PlaceBlockEvent {
    glm::ivec3 position;
    glm::ivec3 face;
    Hand hand;
    int32_t predictedStateId = -1;

    void Dispatch(GlobalState* state) const
    {
        // Cursor: where within `face` the click landed, 0..1 on each axis.
        // RaycastBlocks doesn't track the exact 3D hit point (only the
        // distance along the ray), so this approximates "dead center of the
        // clicked face" instead of real per-pixel precision - fine for
        // which-cell-does-the-new-block-go-in (position+face already answer
        // that), only actually imprecise for the handful of blocks whose
        // placement orientation reads the cursor sub-position too (e.g. a
        // trapdoor's exact hinge pixel), not just position+face+look-yaw.
        glm::vec3 cursor(0.5f, 0.5f, 0.5f);
        if (face.x != 0) cursor.x = face.x > 0 ? 1.0f : 0.0f;
        if (face.y != 0) cursor.y = face.y > 0 ? 1.0f : 0.0f;
        if (face.z != 0) cursor.z = face.z > 0 ? 1.0f : 0.0f;

        SendBlockPlace(state, hand, position, face, cursor);
        SendArmAnimation(state, hand);

        if (predictedStateId >= 0) PredictLocally(state);
    }

private:
    void PredictLocally(GlobalState* state) const
    {
        glm::ivec3 target = position + face;
        if (!state->world->GetBlock(target.x, target.y, target.z).isAir()) return;

        Block block = BlockRegistry::ResolveStateId(predictedStateId);

        // The server refuses a solid block placed inside the player; don't
        // predict one either, or the client would collide with a ghost.
        if (block.hasCollision())
        {
            glm::vec3 feet = state->player->GetPosition();
            glm::vec3 playerMin(feet.x - Player::HALF_WIDTH, feet.y, feet.z - Player::HALF_WIDTH);
            glm::vec3 playerMax(feet.x + Player::HALF_WIDTH, feet.y + Player::HEIGHT, feet.z + Player::HALF_WIDTH);
            glm::vec3 blockMin(target);
            glm::vec3 blockMax = blockMin + glm::vec3(1.0f);
            if (glm::all(glm::lessThan(playerMin, blockMax)) && glm::all(glm::greaterThan(playerMax, blockMin))) return;
        }

        std::vector<SectionKey> dirty = state->world->SetBlock(target, block);
        if (state->lightEngine != nullptr)
        {
            for (const SectionKey& key : state->lightEngine->OnBlockChanged(target, Block{}, block)) dirty.push_back(key);
        }
        if (!dirty.empty())
        {
            std::lock_guard<std::mutex> lock(state->networkInbox.mutex);
            for (const SectionKey& key : dirty) state->networkInbox.dirtySections.push_back(key);
        }
        if (state->blockSounds != nullptr) state->blockSounds->PlayPlace(block);
    }
};

} // namespace Volcano

#endif
