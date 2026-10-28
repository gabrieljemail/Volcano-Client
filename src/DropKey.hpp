#pragma once
#ifndef VOLCANO_DROP_KEY_H
#define VOLCANO_DROP_KEY_H

namespace Volcano {

// Turns the drop key's press/hold/release into "drop one" or "drop the stack":
// a tap drops one item (on release, so a hold can still become a stack), and
// holding it for HOLD_SECONDS drops the whole stack, once. Holding Ctrl drops
// the stack at once, as in vanilla. One instance per place that reads the key
// (the world, the inventory screen), fed every frame it is active.
class DropKeyTracker {
public:
    enum class Result { NONE, ONE, STACK };

    static constexpr float HOLD_SECONDS = 0.35f;

    // `pressed`/`down`/`released`: the key's state this frame (the DropItem,
    // DropItemHold and DropItemRelease actions).
    Result Update(bool pressed, bool down, bool released, bool ctrlHeld, float deltaSeconds)
    {
        if (pressed)
        {
            held = true;
            stackSent = false;
            heldFor = 0.0f;
            if (ctrlHeld)
            {
                stackSent = true;
                return Result::STACK;
            }
        }
        if (!held) return Result::NONE;

        if (down && !released)
        {
            heldFor += deltaSeconds;
            if (!stackSent && heldFor >= HOLD_SECONDS)
            {
                stackSent = true;
                return Result::STACK;
            }
            return Result::NONE;
        }

        // Released (or no longer down): a tap that never became a stack drops one.
        held = false;
        return stackSent ? Result::NONE : Result::ONE;
    }

    // Forget a half-finished press, e.g. when a screen opened mid-hold.
    void Reset() { held = false; stackSent = false; heldFor = 0.0f; }

private:
    bool held = false;
    bool stackSent = false;
    float heldFor = 0.0f;
};

} // namespace Volcano

#endif
