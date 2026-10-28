#include "BlockSounds.hpp"
#include "SoundRegistry.hpp"
#include <algorithm>
#include <random>
#include <variant>
#include <vector>

namespace Volcano {

// Vanilla's volume/pitch per event: step is 0.15 of the block's volume (this
// client plays it at 0.3), break/place use (volume + 1) / 2 at 0.8 pitch, and
// the mining hit is a quarter volume at half pitch.
void BlockSounds::PlayStep(const Block& block) { Play("step", block, 0.3f, 1.0f, true); }
void BlockSounds::PlayBreak(const Block& block) { Play("dig", block, 1.0f, 0.8f, false); }
void BlockSounds::PlayPlace(const Block& block) { Play("dig", block, 1.0f, 0.8f, false); }
void BlockSounds::PlayHit(const Block& block) { Play("step", block, 0.25f, 0.5f, false); }

void BlockSounds::PlaySplash(float speed)
{
    // Vanilla: a quiet splash below 0.25, the heavy one above.
    PlayVariant(speed < 0.25f ? "minecraft:liquid/splash" : "minecraft:liquid/heavy_splash", 0, std::max(speed, 0.15f), 1.0f);
}

void BlockSounds::PlayWaterExit() { PlayVariant("minecraft:liquid/splash", 0, 0.25f, 1.0f); }
void BlockSounds::PlayUnderwaterEnter() { PlayVariant("minecraft:ambient/underwater/enter", 3, 0.6f, 1.0f); }
void BlockSounds::PlayUnderwaterExit() { PlayVariant("minecraft:ambient/underwater/exit", 3, 0.6f, 1.0f); }
void BlockSounds::PlaySwimStroke(float speed) { PlayVariant("minecraft:liquid/swim", 18, std::clamp(speed, 0.1f, 1.0f), 1.0f); }

void BlockSounds::SetUnderwaterLoop(bool active)
{
    if (state->soundRegistry == nullptr || state->audioManager == nullptr) return;

    const std::string name = "minecraft:ambient/underwater/underwater_ambience";
    if (active)
    {
        if (!state->audioManager->IsSoundPlaying(name)) PlayFile(name, 0.3f, 1.0f, true);
    }
    else
    {
        state->audioManager->StopSound(name);
    }
}

void BlockSounds::PlayFile(const std::string& name, float volume, float pitch, bool loop)
{
    if (state->soundRegistry == nullptr || state->audioManager == nullptr) return;

    float masterVolume = static_cast<float>(std::get<uint32_t>(state->config->Get("Audio.Volume", 100u))) / 100.0f;
    state->soundRegistry->Play(state->audioManager, name, volume * masterVolume, pitch, loop);
}

// `count` of 0 plays `base` as is; otherwise picks <base>1..<base><count>.
void BlockSounds::PlayVariant(const std::string& base, int count, float volume, float pitch)
{
    static thread_local std::mt19937 gen{std::random_device{}()};
    std::string name = base;
    if (count > 0) name += std::to_string(std::uniform_int_distribution<int>(1, count)(gen));
    pitch += (std::uniform_real_distribution<float>(0.0f, 1.0f)(gen) - std::uniform_real_distribution<float>(0.0f, 1.0f)(gen)) * 0.4f;
    PlayFile(name, volume, pitch);
}

std::string BlockSounds::MaterialFor(const Block& block) const
{
    std::string texture;
    if (block.visualId != 0)
    {
        texture = BlockRegistry::GetFaceTextureName(block.visualId, 0);
    }
    else
    {
        const BlockRegistry::NonCubeVisual& visual = BlockRegistry::GetNonCubeVisual(block.nonCubeVisualId);
        if (!visual.elements.empty()) texture = visual.elements[0].faceTextures[0];
        else texture = visual.crossTexture;
    }

    auto has = [&](const char* part) { return texture.find(part) != std::string::npos; };

    if (has("sandstone")) return "stone";
    if (has("sand")) return "sand";
    if (has("gravel") || has("dirt") || has("coarse") || has("podzol")) return "gravel";
    if (has("grass") || has("moss") || has("leaves") || has("fern") || has("mycelium")) return "grass";
    if (has("snow")) return "snow";
    if (has("wool") || has("carpet")) return "cloth";
    if (has("coral")) return "coral";
    if (has("log") || has("planks") || has("wood") || has("stem") || has("hyphae") || has("bamboo")
        || has("crafting") || has("bookshelf") || has("barrel")) return "wood";
    return "stone";
}

void BlockSounds::Play(const char* folder, const Block& block, float volume, float pitch, bool randomPitch)
{
    if (state->soundRegistry == nullptr || state->audioManager == nullptr) return;
    if (block.isAir()) return;

    // Variants are <folder>/<material>1.ogg, 2, 3, ... - probe for how many exist.
    std::string base = std::string("minecraft:") + folder + "/" + MaterialFor(block);
    std::vector<std::string> variants;
    for (int i = 1; i <= 8; i++)
    {
        std::string name = base + std::to_string(i);
        if (state->soundRegistry->Has(name)) variants.push_back(name);
    }
    if (variants.empty()) return;

    static thread_local std::mt19937 gen{std::random_device{}()};
    const std::string& sound = variants[std::uniform_int_distribution<size_t>(0, variants.size() - 1)(gen)];
    if (randomPitch) pitch *= std::uniform_real_distribution<float>(0.9f, 1.1f)(gen);
    float masterVolume = static_cast<float>(std::get<uint32_t>(state->config->Get("Audio.Volume", 100u))) / 100.0f;
    state->soundRegistry->Play(state->audioManager, sound, volume * masterVolume, pitch);
}

} // namespace Volcano
