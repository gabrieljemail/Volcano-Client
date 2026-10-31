#pragma once
#ifndef VOLCANO_GUI_SCREENS_WORLD_MANAGER_H
#define VOLCANO_GUI_SCREENS_WORLD_MANAGER_H

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <array>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
#include "../../../Config.hpp"
#include "../../../GlobalState.hpp"
#include "../../../Logger.hpp"
#include "../../../platform/OS.hpp"
#include "../GUIController.hpp"
#include "../models/Button.hpp"
#include "../models/GUIComponent.hpp"
#include "../models/Screen.hpp"

#define WORLD_LIST_FILE std::string("worlds.json")

/**
 * Exposes and implements GUI functionality for managing game files,
 * such as assets and worlds.
 */
namespace Volcano::GUI {

using nlohmann::json;

// The built-in list of server distros that we support downloading.
enum class ServerDistro : uint8_t {
    VANILLA = 0,
    // Modded servers:
    FABRIC = 1, FORGE = 2, NEOFORGE = 3,
    // Patched plugin/API servers, in the order each project forked the last.
    BUKKIT = 4, SPIGOT = 5, PAPER = 6, PURPUR = 7, PUFFERFISH = 8,
};

inline std::string ServerDistroToString(ServerDistro distro)
{
    switch (distro)
    {
        default:
        case ServerDistro::VANILLA:    return "Vanilla";
        case ServerDistro::FABRIC:     return "Fabric";
        case ServerDistro::FORGE:      return "Forge";
        case ServerDistro::NEOFORGE:   return "NeoForge";
        case ServerDistro::BUKKIT:     return "CraftBukkit";
        case ServerDistro::SPIGOT:     return "Spigot";
        case ServerDistro::PAPER:      return "PaperMC";
        case ServerDistro::PURPUR:     return "Purpur";
        case ServerDistro::PUFFERFISH: return "Pufferfish";
    }
}

struct ServerVersion {
    // For 1.21.11, this would be 1, 21, and 11.
    // For the newer 26.x versions (like 26.4), it would be 0, 26, 4.
    uint8_t build = 0;
    uint8_t major = 0;
    uint8_t minor = 0;

    static std::string ToString(ServerVersion self)
    {
        std::string str;
        if (self.build > 0) str.append(std::to_string(self.build) + ".");
        str.append(std::to_string(self.major) + ".");
        str.append(std::to_string(self.minor));
        return str;
    }

    // Accepts "26.1" or "1.21.11"; anything else yields 0.0.0.
    static ServerVersion FromString(const std::string& text)
    {
        std::vector<int> parts;
        size_t start = 0;
        while (start <= text.size())
        {
            size_t dot = text.find('.', start);
            if (dot == std::string::npos) dot = text.size();
            try
            {
                parts.push_back(std::stoi(text.substr(start, dot - start)));
            }
            catch (...)
            {
                return ServerVersion{};
            }
            start = dot + 1;
        }

        auto byte = [](int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); };
        if (parts.size() == 2) return ServerVersion{0, byte(parts[0]), byte(parts[1])};
        if (parts.size() == 3) return ServerVersion{byte(parts[0]), byte(parts[1]), byte(parts[2])};
        return ServerVersion{};
    }
};

// Singleplayer servers are called "worlds" here, but they are still
// just Minecraft servers. Some information here is not necessary but
// used for GUI display of the world, such as server brand.
struct World {
    std::string name;
    std::string dir;
    ServerVersion version;
    ServerDistro distro = ServerDistro::PAPER;
    std::string jvmPath;
    std::string jvmArgs;

    // The server's working directory: "<dir>/<name>".
    std::filesystem::path Path() const
    {
        return std::filesystem::path(dir) / name;
    }

    // Creates a JSON object to save to worlds.json.
    json Serialize() const
    {
        return json{
            {"name", name},
            {"version", ServerVersion::ToString(version)},
            {"dir", dir},
            {"distro", static_cast<uint8_t>(distro)},
            {"jvmPath", jvmPath},
            {"jvmArgs", jvmArgs},
        };
    }

    // Missing or mistyped fields keep their defaults.
    static World Deserialize(const json& entry, GlobalState* state)
    {
        World world;
        FromDefaults(&world, state);
        world.name = entry.value("name", world.name);
        world.version = ServerVersion::FromString(entry.value("version", ServerVersion::ToString(world.version)));
        world.dir = entry.value("dir", world.dir);
        world.distro = static_cast<ServerDistro>(entry.value("distro", static_cast<int>(world.distro)));
        world.jvmPath = entry.value("jvmPath", world.jvmPath);
        world.jvmArgs = entry.value("jvmArgs", world.jvmArgs);
        return world;
    }

    // Sets these values to defaults, inheriting some from settings.json.
    static void FromDefaults(World* self, GlobalState* state)
    {
        self->name = "New world";
        self->version = ServerVersion{0, 26, 1}; // 26.1, our protocol implementation.
        self->dir = "saves";
        self->distro = ServerDistro::PAPER;
        self->jvmPath = std::get<std::string>(state->config->Get("Singleplayer.JavaPath", std::string("java/bin/java.exe")));
        self->jvmArgs = std::get<std::string>(state->config->Get("Singleplayer.JavaArgs", std::string("-Xmx1536M -Xms1G -Xss1M -XX:+AlwaysPreTouch -XX:+UseSerialGC -XX:+UseG1GC -XX:ConcGCThreads=1 -XX:ParallelGCThreads=2 -XX:G1NewSizePercent=40 -XX:MaxGCPauseMillis=50 -XX:+UseStringDeduplication -XX:+UnlockExperimentalVMOptions -XX:+TieredCompilation -XX:CICompilerCount=2 -XX:+DontCompileHugeMethods -XX:+VectorizeSIMD --add-modules=jdk.incubator.vector")));
    }
};

namespace WorldStorage {

inline std::vector<World>& List()
{
    static std::vector<World> worlds;
    return worlds;
}

inline std::filesystem::path ListPath()
{
    return std::filesystem::current_path() / WORLD_LIST_FILE;
}

// Folder names can't hold separators or Windows-reserved characters.
inline std::string SafeName(const std::string& name)
{
    std::string safe;
    for (char c : name)
    {
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32)
            safe.push_back('_');
        else
            safe.push_back(c);
    }
    while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.')) safe.pop_back();
    if (safe.empty()) safe = "New world";
    return safe;
}

} // namespace WorldStorage

// In-memory world list.
inline std::vector<World>* GetWorlds()
{
    return &WorldStorage::List();
}

// Reads worlds.json into the in-memory list. A missing file is an empty list;
// an invalid one is renamed to worlds.json.bad so a later save can't erase it.
inline void LoadWorlds(GlobalState* state)
{
    auto& worlds = WorldStorage::List();
    worlds.clear();

    const std::filesystem::path path = WorldStorage::ListPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;

    try
    {
        std::ifstream file(path);
        json root = json::parse(file);
        if (!root.is_array()) throw std::runtime_error("expected an array");
        for (const json& entry : root)
        {
            if (entry.is_object()) worlds.push_back(World::Deserialize(entry, state));
        }
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("\x1b[90mWorlds:\x1b[0m Could not read worlds.json: ") + e.what());
        worlds.clear();
        std::filesystem::rename(path, path.string() + ".bad", ec);
    }
}

// Syncs worlds.json with the in-memory list. Returns false (and logs) on failure.
inline bool SaveWorlds()
{
    json root = json::array();
    for (const World& world : WorldStorage::List()) root.push_back(world.Serialize());

    std::ofstream file(WorldStorage::ListPath(), std::ios::trunc);
    if (!file)
    {
        Log::Error("\x1b[90mWorlds:\x1b[0m Could not open worlds.json for writing");
        return false;
    }
    file << root.dump(2);
    return static_cast<bool>(file);
}

// Maps a server distro to a download URL. An empty URL means it isn't available yet.
inline const std::array<std::pair<ServerDistro, std::string>, 9> serverDownloads {{
	{ServerDistro::VANILLA,		"https://piston-data.mojang.com/v1/objects/3872a7f07a1a595e651aef8b058dfc2bb3772f46/server.jar"},

	// These server distros download the vanilla server anyway.
	{ServerDistro::FABRIC,		"https://meta.fabricmc.net/v2/versions/loader/26.1/0.20.1/1.1.2/server/jar"},
	{ServerDistro::FORGE,		""},
	{ServerDistro::NEOFORGE,	"https://maven.neoforged.net/releases/net/neoforged/neoforge/26.1.0.19-beta/neoforge-26.1.0.19-beta-installer.jar"},
	{ServerDistro::BUKKIT,		"https://getbukkit.org/craftbukkit/craftbukkit-26.1.jar"},
	{ServerDistro::SPIGOT,		""},
	{ServerDistro::PAPER,		"https://fill-data.papermc.io/v1/objects/1d70b1dab9cf4a6de615209a536f3a45a2186240253c428213ce2188ab95e5f7/paper-26.1.2-74.jar"},
	{ServerDistro::PURPUR,		""},
	{ServerDistro::PUFFERFISH,	""}
}};

inline std::string GetServerDownloadURL(ServerDistro distro)
{
    for (const auto& [candidate, url] : serverDownloads)
    {
        if (candidate == distro) return url;
    }
    return "";
}

// The server won't start until eula.txt says eula=true. Asks the player once, on
// the first launch, and writes it only on a yes. Returns whether it is accepted.
// Blocks on a dialog, so call it from the thread that owns the UI.
inline bool EnsureEula(const World& world)
{
    const std::filesystem::path path = world.Path() / "eula.txt";

    std::ifstream existing(path);
    for (std::string line; std::getline(existing, line);)
    {
        line.erase(std::remove_if(line.begin(), line.end(), [](char c) { return c == ' ' || c == '\r'; }), line.end());
        if (line == "eula=true") return true;
    }
    existing.close();

    const bool agreed = Platform::OS::ShowConsentDialog("Minecraft EULA",
        "Running a Minecraft server means agreeing to Mojang's EULA:\n"
        "https://aka.ms/MinecraftEULA\n\n"
        "Do you agree to the Minecraft EULA?", "Agree");
    if (!agreed) return false;

    std::ofstream file(path, std::ios::trunc);
    file << "# Accepted in Volcano Client by the player.\neula=true\n";
    if (!file)
    {
        Log::Error("\x1b[90mWorlds:\x1b[0m Could not write " + path.string());
        return false;
    }
    return true;
}

// The client can't do Mojang authentication yet, so a world's server has to run with online-mode=false.
// Rewrites that line in server.properties, or creates the file with it; the server adds the rest on first run.
inline bool SetOfflineMode(const World& world)
{
    const std::filesystem::path path = world.Path() / "server.properties";

    std::vector<std::string> lines;
    bool found = false;
    {
        std::ifstream existing(path);
        for (std::string line; std::getline(existing, line);)
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("online-mode=", 0) == 0)
            {
                line = "online-mode=false";
                found = true;
            }
            lines.push_back(line);
        }
    }
    if (!found) lines.push_back("online-mode=false");

    std::ofstream file(path, std::ios::trunc);
    for (const std::string& line : lines) file << line << "\n";
    if (!file)
    {
        Log::Error("\x1b[90mWorlds:\x1b[0m Could not write " + path.string());
        return false;
    }
    return true;
}

// Adds a world to the list (renaming it if the name is taken), creates its
// directory and saves. Returns false if the directory could not be created.
// The server jar is downloaded separately (see DownloadServerJar).
inline bool CreateWorld(World worldMeta)
{
    auto& worlds = WorldStorage::List();
    const std::string baseName = WorldStorage::SafeName(worldMeta.name);

    auto taken = [&](const std::string& name)
    {
        return std::any_of(worlds.begin(), worlds.end(), [&](const World& other)
        {
            return other.dir == worldMeta.dir && other.name == name;
        });
    };
    std::string name = baseName;
    for (int copy = 2; taken(name); copy++) name = baseName + " (" + std::to_string(copy) + ")";
    worldMeta.name = name;

    std::error_code ec;
    std::filesystem::create_directories(worldMeta.Path(), ec);
    if (ec)
    {
        Log::Error("\x1b[90mWorlds:\x1b[0m Could not create " + worldMeta.Path().string() + ": " + ec.message());
        return false;
    }
    SetOfflineMode(worldMeta);

    worlds.push_back(worldMeta);
    return SaveWorlds();
}

// Removes the entry matching dir and name, saves, and deletes its directory.
inline void DeleteWorld(const std::string& dir, const std::string& name)
{
    auto& worlds = WorldStorage::List();
    auto found = std::find_if(worlds.begin(), worlds.end(), [&](const World& world)
    {
        return world.dir == dir && world.name == name;
    });
    if (found == worlds.end()) return;

    // Only delete a folder that is exactly <dir>/<name>, never something outside it.
    const std::filesystem::path path = found->Path();
    const bool contained = WorldStorage::SafeName(name) == name && name != "." && name != ".."
        && path.parent_path() == std::filesystem::path(dir);

    worlds.erase(found);
    SaveWorlds();

    if (!contained)
    {
        Log::Error("\x1b[90mWorlds:\x1b[0m Refusing to delete " + path.string());
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) Log::Error("\x1b[90mWorlds:\x1b[0m Could not delete " + path.string() + ": " + ec.message());
}

inline void Shutdown()
{
    WorldStorage::List().clear();
}

// Downloads the world's server jar to "<world dir>/server.jar". Blocks, so call it from a worker thread.
// `progress(received, total)` runs as data arrives (total is 0 when unknown). Returns false with a reason in `error`.
bool DownloadServerJar(const World& world, std::string& error, const std::function<void(uint64_t, uint64_t)>& progress);

// What the world list's buttons do; the caller owns the connection and the other screens, so they are callbacks.
struct WorldListActions {
    std::function<void(const World&)> play; // Start this world's server and join it.
    std::function<void()> back;
};

// Fills `screen` with the singleplayer world list: Play, Edit and Delete per world, and a form to create one.
void ConfigureWorldScreen(Screen* screen, GlobalState* state, WorldListActions actions);

} // namespace Volcano::GUI

#endif
