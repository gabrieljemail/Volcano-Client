#include "EntitySounds.hpp"
#include "SoundRegistry.hpp"
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

void EntitySounds::PlayHurt(const std::string& entityName, float distance)
{
    if (state->soundRegistry == nullptr || state->audioManager == nullptr) return;
    if (distance >= HEARING_RANGE) return;

    std::vector<std::string> variants;
    if (entityName == "player")
    {
        variants = Variants(*state->soundRegistry, "minecraft:damage/hit");
    }
    else
    {
        // Folder names drop underscores ("polar_bear" -> "polarbear") and a few
        // mobs borrow a relative's sounds.
        std::string folder = entityName;
        if (folder == "mooshroom") folder = "cow";
        else if (folder == "cave_spider") folder = "spider";
        std::string squashed = folder;
        squashed.erase(std::remove(squashed.begin(), squashed.end(), '_'), squashed.end());

        for (const std::string& candidate : { folder, squashed })
        {
            variants = Variants(*state->soundRegistry, "minecraft:mob/" + candidate + "/hurt");
            if (variants.empty()) variants = Variants(*state->soundRegistry, "minecraft:mob/" + candidate + "/hit");
            if (!variants.empty()) break;
        }
    }
    if (variants.empty()) return;

    static thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const std::string& sound = variants[std::uniform_int_distribution<size_t>(0, variants.size() - 1)(gen)];

    // Vanilla's voice pitch is 1.0 +- 0.2, and volume falls off with distance.
    float pitch = 1.0f + (unit(gen) - unit(gen)) * 0.2f;
    float masterVolume = static_cast<float>(std::get<uint32_t>(state->config->Get("Audio.Volume", 100u))) / 100.0f;
    float volume = masterVolume * (1.0f - distance / HEARING_RANGE);
    state->soundRegistry->Play(state->audioManager, sound, volume, pitch);
}

} // namespace Volcano
