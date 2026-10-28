#pragma once
#ifndef VOLCANO_RESOURCES_RESOURCE_MANAGER_H
#define VOLCANO_RESOURCES_RESOURCE_MANAGER_H

#include "ZipArchive.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

namespace Volcano {

struct GlobalState;

// Layered asset lookup: each Graphics.ResourcePacks entry (a .zip or a
// directory in resourcepacks/), highest priority first, then the base pack at
// resources/minecraft. Logical paths are pack-internal, e.g.
// "assets/minecraft/textures/block/stone.png".
//
// Thread safety: lookups take a shared lock, Reload() the exclusive one, so
// any thread may read while one reloads.
class ResourceManager {
public:
    // Registers this instance as Instance() and loads the layers.
    explicit ResourceManager(GlobalState* state);
    ~ResourceManager();

    // Valid after construction; logs and aborts if there is none.
    static ResourceManager& Instance();

    // Contents from the first layer that has logicalPath.
    std::optional<std::vector<uint8_t>> ReadBytes(const std::string& logicalPath) const;
    bool Exists(const std::string& logicalPath) const;

    // Union across layers, de-duplicated, as logical paths. ext like ".png"
    // (empty = any); dir has no trailing slash.
    std::vector<std::string> List(const std::string& dir, const std::string& ext, bool recursive) const;

    // Re-reads Graphics.ResourcePacks and reopens the archives.
    void Reload();

    // Base-layer disk path (resources/minecraft/<logicalPath>) for legacy
    // path-based consumers.
    std::string DiskPath(const std::string& logicalPath) const;

private:
    struct Layer
    {
        std::string root;                // Directory layers: disk root.
        std::unique_ptr<ZipArchive> zip; // Zip layers; null for directories.
    };

    static bool IsSafe(const std::string& logicalPath);
    static void ListDirectory(const std::string& root, const std::string& dir, const std::string& ext,
        bool recursive, std::set<std::string>& out);
    std::string FindPack(const std::string& name);

    GlobalState* state;
    mutable std::shared_mutex mutex;
    std::vector<Layer> layers;
    std::set<std::string> warned;

    static inline ResourceManager* instance = nullptr;
};

} // namespace Volcano

#endif
