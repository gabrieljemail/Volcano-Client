#pragma once
#ifndef VOLCANO_INTERACTION_MANAGER_H
#define VOLCANO_INTERACTION_MANAGER_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <glm/glm.hpp>
#include "../GlobalState.hpp"
#include "../Helpers.hpp"
#include "../InputHandler.hpp"
#include "../inventory/ItemStack.hpp"

namespace Volcano {

struct InteractionManagerConfig {
    // Color of the block selection outline.
    Color4i selectionOutline;
    // Fill of the block selection outline.
    Color4i selectionFill;
};

// One block-shaped raycast hit: which cell, how far along the ray, and
// enough to place a new block against it correctly.
struct TargetedBlock {
    glm::ivec3 position;
    float distance;

    // Which of the 6 cubic faces the ray was looking at, as a unit normal
    // (e.g. (0,1,0) for the top) — always computed against the block's
    // full unit cube, regardless of its real collision shape, so a stair's
    // riser (or any other internal sub-shape face) reports as whichever
    // outer cube face it's part of, not its own shape-specific normal.
    // That keeps "which face did you click" predictable for placement
    // instead of exposing a shape's internal geometry — see RaycastBlocks'
    // own comment.
    glm::ivec3 face;

    // Direction from this block toward wherever the ray was cast from (the
    // player's eye), quantized to the nearest cardinal axis — vanilla's
    // own Direction.getNearest(x,y,z). The primitive most directional
    // blockstates (a furnace/dispenser's "facing", a stair's own facing,
    // ...) are derived from, once block placement actually dispatches
    // something — nothing reads this yet.
    glm::ivec3 relativeDirection;
};

// Owns mainhand/offhand interaction (attacking, and eventually block
// breaking/placing) plus the crosshair's block/entity selection. Runs on
// the Main/Render thread, driven once per frame from
// RenderThread::PollInputs alongside Jump/Hotbar/Camera.
//
// RaycastSelection() walks the camera's look ray against state->world
// (block-by-block, via a DDA voxel walk — not a bounding-box scan) and
// state->entities (a cheap distance reject before a real ray/AABB test),
// keeping whichever of the two the ray reaches first. PrimaryTrigger/
// SecondaryTrigger read targetedBlock/targetedEntityId, but nothing
// dispatches an actual attack/break/use/place yet — this client has no
// outbound packet for any of those.
class InteractionManager {
public:
    explicit InteractionManager(GlobalState* globalState);

    // Runs once per frame (see the class comment for which thread/call
    // site). `inputAllowed` matches RenderThread::PollInputs' own flag —
    // false while a Screen or the chat box has input focus, so a click
    // that's really dismissing a menu doesn't also swing/use.
    void Update(bool inputAllowed);

    // Vanilla's Player::getAttackStrengthScale: 0 right after a swing, 1.0
    // once state->attackStrengthTicker has counted up to a full recovery —
    // recomputed from the live generic.attack_speed attribute every call
    // (see GlobalState::attackStrengthTicker's own comment), not cached.
    // Public since a future HUD attack-cooldown indicator (vanilla has one)
    // would read this too.
    float GetAttackStrengthScale() const;

    // Current block selection target, if any — read by SelectionRenderer
    // once per frame to draw the outline/fill. See RaycastSelection.
    std::optional<TargetedBlock> GetTargetedBlock() const { return targetedBlock; }
    Color4i GetSelectionOutlineColor() const { return selectionOutline; }
    Color4i GetSelectionFillColor() const { return selectionFill; }

    // Called when config needs to be reloaded.
    void SetConfig(const InteractionManagerConfig* config)
    {
        // For now, it's just block selection stuff.
        selectionOutline = config->selectionOutline;
        selectionFill    = config->selectionFill;
    }

private:
    GlobalState* state;

    // Snapshotted each Update() (see RefreshHeldItems) rather than held as
    // pointers into state->inventory — a pointer surviving past its lock
    // would race a future inventory-packet write from NetworkThread.
    ItemStack mainhand;
    ItemStack offhand;

    // True if the offhand item has a secondary action (e.g. a shield).
    // Always false for now — ItemRegistry has no such flag yet.
    bool offhandInteractible = false;

    // True if the mainhand slot is Air and the hand model
    // should render instead.
    bool mainhandEmpty = false;
    // The position and rotation of the hand slot.
    // Updated every frame when the hand is animating.
    Vector3f handModelTransform{};

    // Closest thing RaycastSelection found the camera looking at, if
    // anything — at most one of these is set (see RaycastSelection's own
    // comment on picking whichever the ray reaches first).
    std::optional<TargetedBlock> targetedBlock;
    std::optional<uint32_t> targetedEntityId;

    // A real (non-instant, non-creative) mining hold in progress — see
    // BeginMining/ContinueMining/CancelMining. Absent whenever nothing's
    // being mined: between clicks, during an instant break (creative or
    // hardness-0), or while a real hold's dig timer hasn't started yet.
    struct MiningState {
        glm::ivec3 position;
        glm::ivec3 face;
        int32_t ticksRequired; // See BlockRegistry::MapStateIdBreakTicks.
        std::chrono::steady_clock::time_point startTime;
    };
    std::optional<MiningState> mining;

    // Reach distance, in blocks.
    // Synced with server NBT attributes.
    float reachDistance = 3.0f;
    // Block reach distance, in blocks.
    // Synced with server NBT attributes.
    float blockReachDistance = 4.5f;

    // Color of the block selection outline.
    Color4i selectionOutline{255, 255, 255, 0.8f};
    // Fill of the block selection outline.
    Color4i selectionFill{255, 255, 255, 0.15f};

    // Re-reads mainhand/offhand (and the flags derived from them) from
    // state->inventory under its mutex — see mainhand/offhand's own comment.
    void RefreshHeldItems();

    // Re-reads reachDistance/blockReachDistance from state->attributes —
    // same "hardcoded default until the server actually sends one" story as
    // every other PlayerAttributes-backed value (see TickLoop's own reads).
    void RefreshReach();

    // Primary trigger handler: attack (edge-gated, one dispatch per click),
    // or drive the mining state machine below — called every frame
    // PrimaryAction is held, not just the press edge, since mining needs to
    // keep running across frames. Update() handles the release-edge cancel
    // this doesn't see, since it's simply not called once the button is up.
    void PrimaryTrigger();

    // Starts mining `target`. canInstabuild or a hardness-0 block (flowers,
    // torches, ...) dispatches an instant BreakBlockEvent. An unbreakable
    // block (bedrock, or one never resolved from a real state id) does
    // nothing. Otherwise sends StartDigging and arms `mining` with the real
    // tick count (BlockRegistry::MapStateIdBreakTicks) for ContinueMining.
    void BeginMining(const TargetedBlock& target);

    // Called every frame a hold continues targeting the same block/face
    // `mining` was armed for. Once enough real time has passed (ticksRequired
    // / 20 seconds), dispatches BreakBlockEvent with sendStart=false (the
    // StartDigging already went out from BeginMining) and clears `mining`.
    // No-op (returns early) while still within the required time.
    void ContinueMining();

    // Ends `mining` early without breaking anything: sends CancelDigging (if
    // a hold was actually in progress — a no-op otherwise) and clears it.
    // Called whenever a hold stops being valid: the button was released, the
    // crosshair moved off the block/face being mined, or an entity/nothing
    // is now targeted instead.
    void CancelMining();

    // Secondary is a use, not just a click — split the same way Jump/
    // JumpHold are, but three ways: an instant item (a block, a bucket)
    // only ever needs Start; a continuous one (food, a shield, a bow) needs
    // Hold to keep going and Release to know when it stopped early.
    void SecondaryTriggerStart();
    void SecondaryTriggerHold();
    void SecondaryTriggerRelease();

    // Resolves the camera's look ray against blocks/entities within reach
    // and updates targetedBlock/targetedEntityId (and GlobalState::
    // lookingAtEntity) accordingly. Called per-frame.
    void RaycastSelection();
};

}

#endif
