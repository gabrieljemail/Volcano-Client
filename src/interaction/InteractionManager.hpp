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
#include "../inventory/ItemBehavior.hpp"
#include "../inventory/ItemStack.hpp"
#include "InteractionTypes.hpp"
#include "../TickLoop.hpp"

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

// What kind of hit the last attack was, by vanilla's own rules (Player.
// attack in resources/26.2.zip) — the four hit types, plus a swing that
// didn't target an entity at all. The server makes the same call from its
// own copy of our state and is the one that actually applies damage and
// knockback; this is the client's prediction, used for the parts of a hit
// vanilla handles client-side (the sprint-hit slowdown — see
// InteractionManager::AttackEntity) and exposed for HUD/feedback use.
enum class AttackType : uint8_t {
    Miss,      // Swung at air or a block.
    Normal,    // A plain hit, at whatever the attack cooldown allowed.
    Knockback, // Sprinting + full strength: extra knockback, ends the sprint.
    Critical,  // Falling + full strength, not sprinting, living target: 1.5x damage.
    Sweep,     // Sword, full strength, standing (nearly) still on the ground: hits nearby entities too.
};

// Owns mainhand/offhand interaction (attacking, and eventually block
// breaking/placing) plus the crosshair's block/entity selection. Runs on
// the Main/Render thread, driven once per frame from
// RenderThread::PollInputs alongside Jump/Hotbar/Camera.
//
// RaycastSelection() walks the camera's look ray against state->world
// (block-by-block, via a DDA voxel walk — not a bounding-box scan) and
// state->entities (a cheap distance reject before a real ray/AABB test),
// keeping whichever of the two the ray reaches first. A primary click on a
// targeted entity attacks it (PrimaryTriggerStart/AttackEntity); block
// breaking and secondary use/place actions dispatch serverbound packets.
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
    //
    // `partialTicks` is vanilla's own parameter: an attack reads the scale
    // half a tick ahead (0.5), the same way Player.attack does.
    float GetAttackStrengthScale(float partialTicks = 0.0f) const;

    // What the most recent primary click did — see AttackType.
    AttackType GetLastAttackType() const { return lastAttackType; }

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
    uint8_t selectedSlot = 0;

    // True if the offhand item has a continuous use (e.g. a shield) and the
    // mainhand doesn't claim the click first. Refreshed with the snapshot.
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

    // Vanilla's hold-to-repeat pacing: destroyDelay (5 ticks after any break)
    // and rightClickDelay (4 ticks between uses). A fresh click skips it.
    static constexpr std::chrono::milliseconds BREAK_REPEAT_DELAY{250};
    static constexpr std::chrono::milliseconds PLACE_REPEAT_DELAY{200};
    std::chrono::steady_clock::time_point lastBreakTime{};
    std::chrono::steady_clock::time_point lastPlaceTime{};
    bool primaryPressedThisFrame = false;

    // Reach distance, in blocks.
    // Synced with server NBT attributes.
    float reachDistance = 3.0f;
    // Block reach distance, in blocks.
    // Synced with server NBT attributes.
    float blockReachDistance = 4.5f;

    AttackType lastAttackType = AttackType::Miss;

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

    // Primary trigger, split like secondary below: Start fires once per
    // click (PrimaryActionPress) and is where attacks happen — vanilla only
    // attacks on a click, never on a held button, which is what makes the
    // attack cooldown something you time yourself. Hold (PrimaryAction) is
    // where the mining state machine below keeps running across frames,
    // since mining needs to keep going for as long as the button stays
    // down. Update() handles the release-edge cancel neither of these sees,
    // since Hold simply isn't called once the button is up.
    void PrimaryTriggerStart();
    void PrimaryTriggerHold();

    // Sends the attack and swing for one click on `entityId`, classifies
    // the hit (see AttackType), and applies what vanilla applies to the
    // attacker client-side.
    void AttackEntity(uint32_t entityId);

    // Vanilla's hit-type rules, given the attack strength, our movement
    // state, and whether the target is a living entity (crits need one).
    AttackType ClassifyAttack(float strength, const TickLoop::MovementSnapshot& movement, bool targetIsLiving) const;

    // Starts mining `target` (after BREAK_REPEAT_DELAY when held). Instabuild
    // or hardness-0 blocks break instantly; unbreakable ones do nothing;
    // anything else sends StartDigging and arms `mining` for ContinueMining.
    void BeginMining(const TargetedBlock& target);
    // Ticks to break `block` with the held item: Block::breakTicks (bare
    // hand) scaled by tool tier/type. -1 unbreakable, 0 instant.
    int32_t BreakTicksFor(const Block& block) const;

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

    // Mirrors `mining` into GlobalState::miningTicksRequired/miningStartNs
    // for the tick thread's crosshair indicator.
    void PublishMining();

    // Secondary is a use, not just a click — split the same way Jump/
    // JumpHold are, but three ways: an instant item (a block, a bucket)
    // only ever needs Start plus Hold's own repeat (see DispatchUse); a
    // continuous one (food, a shield, a bow) needs Hold to keep going and
    // Release to know when it stopped early.
    void SecondaryTriggerStart();
    void SecondaryTriggerHold();
    void SecondaryTriggerRelease();

    // The click dispatch SecondaryTriggerStart/Hold share. Vanilla's order:
    // entity interact, then block use (block entity or place), then, if
    // neither consumed the click, the held item's own use (equip armor, eat,
    // raise a shield, draw a bow). Hold repeats it, paced by PLACE_REPEAT_DELAY.
    void DispatchUse();

    // The item-use half of DispatchUse: picks the hand (mainhand unless it
    // has nothing to do, then the offhand), gates eating on hunger, sends
    // UseItem and, for a continuous use, starts tracking it in `usingState`.
    void UseHeldItem();

    // Ends a continuous use early: sends ReleaseUseItem and clears the state.
    void StopUsingItem();

    // Per-frame upkeep of `usingState`: ends the use when the button came up,
    // the held item changed, or a timed use (food) ran its course.
    void UpdateUsingItem(bool inputAllowed);

    void PublishUsing();

    // The item in `hand`, from the per-frame snapshot.
    const ItemStack& HeldItem(Hand hand) const { return hand == Hand::Main ? mainhand : offhand; }

    // A continuous use in progress — see UseHeldItem.
    struct UsingState {
        Hand hand;
        int32_t itemId;
        uint8_t hotbarSlot;
        UseAction action;
        int32_t useTicks; // 0 = until released.
        std::chrono::steady_clock::time_point startTime;
    };
    std::optional<UsingState> usingState;

    // Resolves the camera's look ray against blocks/entities within reach
    // and updates targetedBlock/targetedEntityId (and GlobalState::
    // lookingAtEntity) accordingly. Called per-frame.
    void RaycastSelection();
};

}

#endif
