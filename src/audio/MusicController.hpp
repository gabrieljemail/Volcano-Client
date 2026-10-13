#pragma once
#ifndef VOLCANO_MUSIC_CONTROLLER_H
#define VOLCANO_MUSIC_CONTROLLER_H

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <ranges>
#include <random>
#include <string_view>
#include "../GlobalState.hpp"
#include "../Logger.hpp"
#include "SoundRegistry.hpp"

namespace Volcano {

constexpr std::string_view MUSIC_CATEGORIES[] = {
    "MainMenu",
    "GenericSurvival",
    "Creative",
    "Nether",
    "TheEnd"
};

class MusicController {
public:
    MusicController(GlobalState* globalState, SoundRegistry* soundRegistry)
        : state(globalState), registry(soundRegistry)
    {
        // Resolve each configured song to its registry name and load it.
        for (std::string_view categoryName : MUSIC_CATEGORIES)
        {
            std::string category(categoryName);
            std::string songs = std::get<std::string>(state->config->Get(
                "Audio.Music.Songs." + category,
                ""
            ));
            std::vector<std::string>& list = music[category];
            for (const auto subrange : songs | std::views::split(','))
            {
                std::string word(subrange.begin(), subrange.end());
                if (word.empty()) continue;

                // Songs are decoded lazily in Tick(); decoding them all
                // here cost gigabytes of RAM and a long startup stall.
                std::string name = registry->FindByStem("minecraft:music/", word);
                if (name.empty())
                {
                    Log::Error("Music: could not find song " + word);
                    continue;
                }
                list.push_back(name);
            }
        }
    }
    ~MusicController() = default;

    void Tick()
    {
        std::string context = IdentifyContext();

        // Cut the song short when the context changes (e.g. entering the Nether).
        if (!currentSong.empty() && context != currentContext)
        {
            state->audioManager->StopSound(currentSong);
            currentSong.clear();
        }
        currentContext = context;

        // Only count the gap between songs while nothing is playing.
        if (!currentSong.empty() && state->audioManager->IsSoundPlaying(currentSong))
        {
            // Keeps the volume sliders live mid-song.
            state->audioManager->SetSoundVolume(currentSong, CurrentVolume());
            return;
        }
        currentSong.clear();

        // Wall-clock gap, since this runs every frame rather than every tick.
        auto now = std::chrono::steady_clock::now();
        if (!wasIdle)
        {
            idleSince = now;
            wasIdle = true;
        }
        if (now - idleSince < gap) return;
        wasIdle = false;

        const std::vector<std::string>& songs = music[context];
        if (songs.empty()) return;

        // Never repeat the previous song when there is a choice.
        std::uniform_int_distribution<size_t> dist(0, songs.size() - 1);
        std::string song = songs[dist(gen)];
        for (int tries = 0; tries < 8 && songs.size() > 1 && song == lastSong; tries++)
        {
            song = songs[dist(gen)];
        }
        lastSong = song;

        if (registry->Play(state->audioManager, song, CurrentVolume(), 1.0f))
        {
            currentSong = song;
        }
    }

private:
    GlobalState* state;
    SoundRegistry* registry;
    // The gap between songs. The first song starts right away.
    std::chrono::milliseconds gap{4500};
    std::chrono::steady_clock::time_point idleSince{};
    bool wasIdle = true;
    // Mix in the clock in case random_device is deterministic on this runtime.
    std::mt19937 gen{static_cast<std::mt19937::result_type>(
        std::random_device{}() ^
        static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()))};
    std::string currentSong;
    std::string lastSong;
    std::string currentContext;
    // A mapping of gameplay categories to a list of registry sound names.
    // Example structure:
    // {
    //   "MainMenu": [
    //     "minecraft:music/game/ebb"
    //   ]
    // }
    std::unordered_map<std::string, std::vector<std::string>> music;

    float CurrentVolume()
    {
        float musicVolume = static_cast<float>(std::get<uint32_t>(state->config->Get("Audio.Music.Volume", 40u))) / 100.0f;
        float masterVolume = static_cast<float>(std::get<uint32_t>(state->config->Get("Audio.Volume", 100u))) / 100.0f;
        return musicVolume * masterVolume;
    }

    std::string IdentifyContext()
    {
        // Anything but an active session counts as the main menu.
        if (state->connectionPhase.load() != ConnectionPhase::Playing) return "MainMenu";

        std::string dimension;
        {
            std::lock_guard<std::mutex> lock(state->currentDimensionMutex);
            dimension = state->currentDimension;
        }
        if (dimension == "minecraft:the_nether") return "Nether";
        if (dimension == "minecraft:the_end") return "TheEnd";

        if (state->localGameMode.load() == 1) return "Creative";
        return "GenericSurvival";
    }
};

} // namespace Volcano

#endif
