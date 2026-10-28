#pragma once
#ifndef VOLCANO_BLOCK_SOUNDS_H
#define VOLCANO_BLOCK_SOUNDS_H

#include <string>
#include "../GlobalState.hpp"
#include "../renderer/terrain/models/BlockRegistry.hpp"

namespace Volcano {

// Block sounds the way vanilla's SoundType plays them: step and hit come from
// sounds/step, break and place from sounds/dig. The client has no per-block
// sound table, so the material is guessed from the block's texture name.
class BlockSounds {
public:
    explicit BlockSounds(GlobalState* state) : state(state) {}

    void PlayStep(const Block& block);
    void PlayBreak(const Block& block);
    void PlayPlace(const Block& block);
    // The repeating tap while a block is being mined.
    void PlayHit(const Block& block);

    // Water sounds, from sounds/liquid and sounds/ambient/underwater.
    // `speed` is vanilla's splash scale (0-1 from the entry velocity).
    void PlaySplash(float speed);
    void PlayWaterExit();
    void PlayUnderwaterEnter();
    void PlayUnderwaterExit();
    // One swim stroke; `speed` as for PlaySplash.
    void PlaySwimStroke(float speed);
    // Starts or stops the looping underwater ambience.
    void SetUnderwaterLoop(bool active);

private:
    void PlayFile(const std::string& name, float volume, float pitch, bool loop = false);
    void PlayVariant(const std::string& base, int count, float volume, float pitch);
    std::string MaterialFor(const Block& block) const;
    void Play(const char* folder, const Block& block, float volume, float pitch, bool randomPitch);

    GlobalState* state;
};

} // namespace Volcano

#endif
