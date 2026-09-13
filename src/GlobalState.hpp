#pragma once
#include "models/GUIWindow.hpp"
#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <atomic>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include "renderer/models/Mesh.hpp"
#include "renderer/entity/models/Entity.hpp"
#include "renderer/gui/models/GUIWindow.hpp"
#include "renderer/terrain/models/Chunk.hpp"
#include "renderer/terrain/models/World.hpp"
#include "InputHandler.hpp"
#include "Player.hpp"
#include "PlayerAttributes.hpp"
#include "Config.hpp"

namespace Volcano {

// Window mode cycled by F11 (VulkanInit.hpp's ToggleWindowMode). Exclusive
// fullscreen is a real mode here so the enum/cycle shape doesn't need to
// change when it's enabled, but ToggleWindowMode skips it for now.
enum class WindowMode : uint8_t {
    Windowed,
    BorderlessFullscreen,
    ExclusiveFullscreen,
};

// Defined in TickLoop.hpp, which includes this header (not the other way
// around) since it needs the full GlobalState — a forward declaration is
// enough here for a pointer member.
class TickLoop;

// Defined in network/Connection.hpp. Forward-declared here (rather than
// including that header, which would pull asio into every GlobalState.hpp
// includer) purely so GlobalState::activeConnection below can hold a
// pointer to it.
class Connection;

struct Resolution {
    uint16_t x;
    uint16_t y;
};

// Handoff point from the network thread to the main thread. The network
// thread parses packets into plain-data Chunks and never touches World or
// renderList directly — the main thread (which already owns renderList and
// calls the Vulkan-touching ChunkMesher) drains this every loop iteration,
// meshes/inserts each chunk, and applies the spawn position once.
struct NetworkInbox {
    std::mutex mutex;
    std::queue<std::unique_ptr<Chunk>> chunks;
    // Carries the initial spawn AND every later teleport the server sends
    // (Player Position, 0x48). Latest wins if two land before MeshingThread
    // drains it — an outdated correction isn't worth applying.
    std::optional<glm::vec3> spawnPosition;
};

struct GlobalState {
    // Owns the client's settings — loaded from settings.json (next to the
    // executable) by LoadSettings() below. config points at configStorage
    // once that's done; every other piece of client code reads settings
    // through this pointer rather than hardcoded constants.
    Config configStorage;
    Config* config{nullptr};

    InputHandler* input;
    Player* player;
    GLFWwindow* window;
    World* world;
    PlayerAttributes* attributes;
    TickLoop* tickLoop;

    // Current state:
    bool shouldClose{false};
    Resolution resolution;
    uint16_t targetFPS{60};
    uint16_t currentFPS{0};
    std::vector<Mesh> renderList = {};
    // Guards renderList itself (not the Meshes' Vulkan handles, which are
    // never mutated after being placed in the slab buffers) — the main
    // thread appends to it from DrainNetworkInbox as chunks stream in,
    // while RenderThread iterates it every frame on its own thread. Without
    // this, a push_back that reallocates while the render thread is mid-
    // iteration is a use-after-free with no exception and no log line, just
    // a sudden crash.
    std::mutex renderListMutex;

    // Non-cubic meshes (transparent full cubes, partial-volume shapes, and
    // cross-shaped plants — see NonCubicMesher) for RenderThread's separate
    // non-cubic pass. One entry per chunk, same as renderList, populated
    // alongside it by MeshingThread and guarded the same way; kept as a
    // separate vector (rather than folded into renderList) because it's
    // drawn with a different pipeline (nonCubicPipeline).
    std::vector<Mesh> nonCubicRenderList = {};
    std::mutex nonCubicRenderListMutex;

    // Entities tracked for EntityRenderer's batched draw pass, keyed by
    // network entity ID for O(1) update/remove on every Move/Teleport/
    // Remove Entities packet. Populated directly by NetworkClient's Play
    // packet loop (Entity has no Vulkan handles, unlike Mesh/renderList, so
    // there's no need to hand it off through a main-thread-only inbox first)
    // — guarded by entitiesMutex the same way renderList is guarded against
    // RenderThread, since NetworkClient runs on its own thread too.
    std::unordered_map<uint32_t, Entity> entities = {};
    std::mutex entitiesMutex;

    NetworkInbox networkInbox;

    // The live Play-session connection, published by NetworkThread right
    // before it starts pumping the Play packet loop and cleared again once
    // that loop returns — nullptr the rest of the time (not yet connected,
    // still in Login/Configuration, or disconnected). Lets
    // NetworkClient::SendChatMessage (called from the render thread, via
    // GUIController's chat input box) reach the socket without NetworkThread
    // having to expose its own NetworkClient instance. Connection::SendPacket
    // is itself safe to call concurrently with the network thread's own use
    // of the same Connection — see its own comment.
    std::atomic<Connection*> activeConnection{nullptr};

    // Set once by MeshingThread, after it has synced TickLoop's cached
    // position and inserted the specific chunk the player is spawning into
    // (not just any chunk — the server has no obligation to send that one
    // first); read every frame by TickLoop::Advance (RenderThread) to hold
    // off gravity/collision until there's an actual floor to land on —
    // otherwise the player free-falls through the still-empty world from the
    // moment the app starts, before any chunk has loaded. Must be set only
    // after all of MeshingThread's other writes for this transition, since
    // RenderThread treats seeing it become true as permission to start
    // reading/writing TickLoop's position state.
    std::atomic<bool> worldReady{false};

    glm::vec3 cameraPosition{0.0f, 0.0f, 5.0f};

    // Set by VulkanInit's GLFW framebuffer-size callback (main thread) and
    // consumed by RenderThread (its own thread) to recreate the swapchain —
    // atomics so no lock is needed across that handoff.
    std::atomic<bool> framebufferResized{false};
    std::atomic<int> pendingFramebufferWidth{0};
    std::atomic<int> pendingFramebufferHeight{0};

    // Loads settings.json (next to the executable), creating it with
    // defaults on first run, and wires config up to point at it. Must run
    // before anything below reads a setting through state->config — in
    // particular before VulkanInit's Init(), which needs Window.Width/Height.
    void LoadSettings()
    {
        configStorage.Load();
        config = &configStorage;

        resolution.x = config->Get<uint16_t>("Window.Width", 854);
        resolution.y = config->Get<uint16_t>("Window.Height", 480);
        targetFPS = config->Get<uint16_t>("Graphics.TargetFPS", 60);
    }
};

}

#endif
