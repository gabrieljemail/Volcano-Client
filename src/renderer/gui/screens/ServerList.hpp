#pragma once
#ifndef VOLCANO_GUI_SCREENS_SERVER_LIST_H
#define VOLCANO_GUI_SCREENS_SERVER_LIST_H

#include <functional>
#include <string>
#include <vector>
#include "../models/Screen.hpp"

#define SERVER_LIST_FILE std::string("servers.json")

namespace Volcano::GUI {

struct ServerEntry {
    std::string name = "Minecraft server";
    std::string address; // "host" or "host:port"
};

// Reads servers.json (next to the exe): an array of {"name", "address"}.
// A missing file is created empty. An invalid one yields an empty list and is
// renamed to servers.json.bad first, so a typo can't be saved over.
std::vector<ServerEntry> GetServers();

// Writes the list back to servers.json. Returns false (and logs) on failure.
bool SaveServers(const std::vector<ServerEntry>& servers);

// What the screen's buttons do; the caller (VolcanoClient.cpp) owns the
// connection and the other screens, so they are callbacks.
struct ServerListActions {
    std::function<void(const ServerEntry&)> join; // Join on a saved server.
    std::function<void()> directConnect;          // Type an address by hand.
    std::function<void()> back;
};

// The multiplayer screen: the saved servers with Join and Remove, a form to
// add one, Direct Connect and Back. Edits are saved to servers.json at once.
Screen* CreateServerList(ServerListActions actions);

} // namespace Volcano::GUI

#endif
