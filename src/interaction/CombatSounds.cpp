#include "CombatSounds.hpp"
#include "InteractionManager.hpp"
#include "../audio/SoundRegistry.hpp"
#include <algorithm>
#include <random>
#include <variant>
#include <vector>

namespace Volcano {

namespace {

constexpr float HEARING_RANGE = 16.0f;

// Numbered variants (<base>1, <base>2, ...) that exist in the registry.
std::vector<std::string> Variants(const SoundRegistry& registry, const std::string& base)
{
    std::vector<std::string> found;
    for (int i = 1; i <= 8; i++)
    {
        std::string name = base + std::to_string(i);
        if (registry.Has(name)) found.push_back(name);
    }
    return found;
}

} // namespace

void CombatSounds::PlayVariant(const std::string& base, float volume, float pitchSpread)
{
    if (state->soundRegistry == nullptr || state->audioManager == nullptr) return;

    std::vector<std::string> variants = Variants(*state->soundRegistry, base);
    if (variants.empty() && state->soundRegistry->Has(base)) variants.push_back(base);
    if (variants.empty()) return;

    static thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const std::string& sound = variants[std::uniform_int_distribution<size_t>(0, variants.size() - 1)(gen)];

    float pitch = 1.0f + (unit(gen) - unit(gen)) * pitchSpread;
    float masterVolume = static_cast<float>(std::get<uint32_t>(state->config->Get("Audio.Volume", 100u))) / 100.0f;
    state->soundRegistry->Play(state->audioManager, sound, volume * masterVolume, pitch);
}

void CombatSounds::PlayAttack(AttackType type, bool fullStrength)
{
    switch (type)
    {
    case AttackType::Knockback: PlayVariant("minecraft:entity/player/attack/knockback", 1.0f, 0.1f); break;
    case AttackType::Critical:  PlayVariant("minecraft:entity/player/attack/crit", 1.0f, 0.1f); break;
    case AttackType::Sweep:     PlayVariant("minecraft:entity/player/attack/sweep", 1.0f, 0.1f); break;
    case AttackType::Normal:
        PlayVariant(fullStrength ? "minecraft:entity/player/attack/strong" : "minecraft:entity/player/attack/weak", 1.0f, 0.1f);
        break;
    case AttackType::Miss: break;
    }
}

void CombatSounds::PlaySpear(const std::string& itemName, bool landedHit)
{
    // Only the wooden spear has its own folder.
    std::string folder = itemName.starts_with("wooden_") ? "minecraft:item/spear/wood/" : "minecraft:item/spear/";
    PlayVariant(folder + "attack", 1.0f, 0.1f);
    if (landedHit) PlayVariant(folder + "hit", 1.0f, 0.1f);
}

void CombatSounds::PlayMaceSmash(float fallDistance, bool grounded)
{
    if (fallDistance > 5.0f) PlayVariant("minecraft:item/mace/smash_ground_heavy", 1.0f, 0.0f);
    else if (grounded) PlayVariant("minecraft:item/mace/smash_ground", 1.0f, 0.0f);
    else PlayVariant("minecraft:item/mace/smash_air", 1.0f, 0.0f);
}

void CombatSounds::PlayArrowShot(float distance)
{
    if (distance >= HEARING_RANGE) return;
    // Vanilla shoots at 1.0 volume, pitch 1/(rand*0.4+1.2) + charge/2.
    PlayVariant("minecraft:random/bow", 1.0f - distance / HEARING_RANGE, 0.15f);
}

} // namespace Volcano
