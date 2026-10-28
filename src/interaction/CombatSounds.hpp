#pragma once
#ifndef VOLCANO_COMBAT_SOUNDS_H
#define VOLCANO_COMBAT_SOUNDS_H

#include <cstdint>
#include <string>
#include "../GlobalState.hpp"

namespace Volcano {

enum class AttackType : uint8_t;

// Melee hit, spear, mace and arrow sounds. Safe to call from any thread
// (everything funnels into SoundRegistry::Play).
class CombatSounds {
public:
    explicit CombatSounds(GlobalState* state) : state(state) {}

    // The sound for a landed hit of `type`; a Normal hit is "strong" at full
    // attack strength, "weak" below it. Misses make no sound.
    void PlayAttack(AttackType type, bool fullStrength);

    // `itemName` is a spear ("wooden_spear", ...). A landed hit adds the hit
    // sound over the swing.
    void PlaySpear(const std::string& itemName, bool landedHit);

    // A mace hit that smashes (falling). Louder and heavier past 5 blocks.
    void PlayMaceSmash(float fallDistance, bool grounded);

    // An arrow appeared `distance` blocks from the listener, from anyone.
    void PlayArrowShot(float distance);

private:
    GlobalState* state;

    void PlayVariant(const std::string& base, float volume, float pitchSpread);
};

} // namespace Volcano

#endif
