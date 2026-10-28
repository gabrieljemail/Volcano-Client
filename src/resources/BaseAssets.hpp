#pragma once
#ifndef VOLCANO_RESOURCES_BASE_ASSETS_H
#define VOLCANO_RESOURCES_BASE_ASSETS_H

#include <functional>
#include <string>

namespace Volcano::BaseAssets {

// The version whose assets the client downloads, and the folder they go in (ResourceManager's base layer).
constexpr const char* VERSION = "26.2";
constexpr const char* DIRECTORY = "resources/minecraft";

// What the user is told, and what they agree to, before anything is fetched.
constexpr const char* NOTICE =
    "Volcano Client needs the game's textures, models, sounds and data to run. They are downloaded directly from "
    "Mojang's servers and are not included with this program.\n\n"
    "We are not affiliated with or sponsored by Mojang or Microsoft in any way, and the use of this feature means you "
    "are responsible for owning a valid license for the game and making sure you are allowed to download this content. "
    "By choosing \"I Agree, Download\", you accept full responsibility over whether you have the appropriate "
    "permission and right to download these assets, which are a copyright of Mojang.";
constexpr const char* AGREE_LABEL = "I Agree, Download";

// Whether the base pack is on disk: the version file and the sounds, which are the last things written.
bool Installed();

// Called with 0..1 and what is happening, from the calling thread.
using ProgressFn = std::function<void(float progress, const std::string& status)>;

// Downloads the client's assets from Mojang (the version list, then the client jar with its hash checked, then the
// sound files) into DIRECTORY. Blocks, and may be run again to resume: files already on disk are kept. Returns
// false with the reason in `error`.
bool Download(const ProgressFn& progress, std::string& error);

} // namespace Volcano::BaseAssets

#endif
