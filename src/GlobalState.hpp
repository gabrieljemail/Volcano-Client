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
#include "SystemInfo.hpp"
#include "inventory/InventoryManager.hpp"

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

// Defined in interaction/InteractionManager.hpp, which includes this header
// (not the other way around) since it needs the full GlobalState — same
// forward-declaration reason as TickLoop above.
class InteractionManager;

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
    InteractionManager* interaction;

    // CPU/GPU/driver/API info for the debug overlay — see SystemInfo's own
    // comment for the detection story and why there's no GPU compute-unit
    // count. DetectCPU() runs first thing in main(); DetectGPU() runs once
    // VulkanInit has a physical device to read properties off of.
    SystemInfo systemInfo;

    // Vanilla's "attack strength" ticker: counts UP once per fixed tick
    // (TickLoop::FixedStep) since the last swing, instead of a cooldown that
    // counts down. InteractionManager::GetAttackStrengthScale divides this
    // by a ticks-to-full-charge figure it recomputes from the live
    // generic.attack_speed attribute every call rather than caching it at
    // swing time — that's what lets swapping weapons mid-recovery actually
    // change how long the recovery takes, matching vanilla's own
    // Player::getAttackStrengthScale/resetAttackStrengthTicker.
    std::atomic<uint32_t> attackStrengthTicker{0};

    // Player inventory (hotbar/main inventory/armor/offhand slot groups) —
    // held directly rather than behind a pointer, same as networkInbox below,
    // since there's exactly one per session. Not yet populated by anything:
    // no packet handling for Window Items/Set Container Slot exists yet, so
    // this is currently just the storage those will write into and the
    // future inventory/hotbar/armor GUI will read from — see its own header
    // for the concurrency convention (a mutex member right alongside the
    // data, matching entities/entitiesMutex and playerList/playerListMutex
    // below).
    InventoryManager inventory;

    // Current state:
    bool shouldClose{false};
    Resolution resolution;
    uint16_t targetFPS{60};
    uint16_t currentFPS{0};

    // The swapchain's actual active VkPresentModeKHR, cast down to a byte —
    // fits every mode this client can request (FIFO=2/IMMEDIATE=0, per
    // Graphics.VSync — see VulkanInit's DesiredPresentMode()) with room to
    // spare; only the SHARED_* modes (unused here) don't fit in a byte.
    // Set by VulkanInit whenever the swapchain is (re)created — vk-bootstrap
    // can silently fall back to a different mode than requested if the
    // surface doesn't support it, so this is what was actually granted, not
    // just what Graphics.VSync asked for. Same thread-ownership story as
    // resolution/targetFPS above: only VulkanInit (Main/Render thread)
    // writes it.
    uint8_t presentMode{0};
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

    // Whether the crosshair is currently over an attackable entity — the
    // tracking field GUIController::RenderCrosshair reads to pick
    // crosshair-entity.png over the plain crosshair.png (vanilla's own
    // attack-indicator swap). Nothing sets this yet: there's no
    // block/entity raycast in this client, against `entities` above or
    // otherwise — see STATE.md's tasks. Stays false (plain crosshair only)
    // until one exists to write it. Atomic rather than mutex-guarded since
    // whatever raycast eventually writes it will most naturally run on the
    // render thread already reading it, but nothing should have to assume
    // that in the meantime.
    std::atomic<bool> lookingAtEntity{false};

    // Tab-list names, keyed by hex-formatted UUID (see NetworkClient's
    // FormatUuid) rather than the raw 16-byte array — same
    // write-from-network/read-from-render split and map+mutex convention as
    // entities/entitiesMutex above. Populated by the Player Info Update/
    // Remove packets in NetworkClient::RunPlayLoop.
    std::unordered_map<std::string, std::string> playerList = {};
    std::mutex playerListMutex;

    // Last values reported by the server's Set Health packet (see
    // NetworkClient's PlayS2C::SetHealth handler) — read every frame by
    // GUIController::RenderPlayerStatusBars on the render thread, written
    // by the network thread the same way entities/entitiesMutex above is.
    // Vanilla's own ranges (health 0-20, food 0-20, saturation 0-5, though
    // saturation can technically exceed 5) aren't enforced here; this just
    // stores whatever the server actually sends.
    float health{20.0f};
    int32_t food{20};
    float saturation{5.0f};
    std::mutex healthMutex;

    NetworkInbox networkInbox;

    // The live Play-session connection, published by NetworkThread right
    // before it starts pumping the Play packet loop and cleared again once
    // that loop returns — null the rest of the time (not yet connected,
    // still in Login/Configuration, or disconnected). Lets
    // NetworkClient::SendChatMessage (called from the render thread, via
    // GUIController's chat input box) reach the connection without
    // NetworkThread having to expose its own NetworkClient instance.
    //
    // A shared_ptr rather than a raw pointer because the Connection is owned
    // by a NetworkClient that NetworkThread destroys as soon as the session
    // ends. With a raw pointer, another thread could load it, pass the null
    // check, be descheduled while the app quits, and come back to a
    // destroyed object — clearing the field afterward doesn't help, since
    // the load already happened. Loading a shared_ptr copy (see
    // SendChatMessage) keeps the connection alive for exactly as long as
    // that caller is still using it. NetworkThread publishes an aliasing
    // shared_ptr that owns the whole NetworkClient — see its own comment.
    //
    // Cross-thread callers must use Connection::QueuePacket, never
    // SendPacket — see Connection's own comment for why.
    std::atomic<std::shared_ptr<Connection>> activeConnection{};

    // Set once by MeshingThread, after it has synced TickLoop's cached
    // position and inserted the specific chunk the player is spawning into
    // (not just any chunk — the server has no obligation to send that one
    // first); read every call by TickLoop::Tick (NetworkThread) to hold
    // off gravity/collision until there's an actual floor to land on —
    // otherwise the player free-falls through the still-empty world from the
    // moment the app starts, before any chunk has loaded. Must be set only
    // after all of MeshingThread's other writes for this transition, since
    // NetworkThread treats seeing it become true as permission to start
    // reading/writing TickLoop's position state.
    std::atomic<bool> worldReady{false};

    // GLFW only guarantees glfwSetInputMode/glfwGetWindowAttrib are safe to
    // call from the thread that created the window — the main (GLFW-owning)
    // thread here, not the render thread GUIController and
    // NotifyFramePresented actually run on. Calling them cross-thread was
    // undefined behavior and the real cause of the pause screen's cursor
    // getting warped back to center every frame (not, as first suspected, a
    // Remote Desktop quirk). Those call sites now store a request here
    // instead; the main loop in VolcanoClient.cpp applies it. -1 = no
    // pending request. windowFocused mirrors real focus state, kept current
    // by WindowFocusCallback (main thread, safe to read GLFW from) so
    // NotifyFramePresented can check focus without calling GLFW itself.
    std::atomic<int> pendingCursorMode{-1};
    std::atomic<bool> windowFocused{false};

    // Set by NetworkThread when a session ends unexpectedly (kicked,
    // connection dropped, login failure) rather than via a deliberate
    // app-level stop — polled once per frame by the main GLFW loop in
    // VolcanoClient.cpp, which resets world/render state and reopens the
    // connect screen with disconnectReason shown. Guarded by
    // disconnectMutex since std::string isn't safe to share via the atomic
    // bool alone.
    std::atomic<bool> disconnected{false};
    std::mutex disconnectMutex;
    std::string disconnectReason;

    void ReportDisconnect(const std::string& reason)
    {
        {
            std::lock_guard<std::mutex> lock(disconnectMutex);
            disconnectReason = reason;
        }
        disconnected.store(true);
    }

    // Set by NetworkClient on PlayS2C::PlayerCombatKill — polled by
    // GUIController::Update() (render thread), which shows a death screen
    // with deathMessage and a Respawn button. Same mutex-for-the-string,
    // atomic-for-the-flag pattern as disconnected/disconnectReason above.
    std::atomic<bool> playerDied{false};
    std::mutex deathMutex;
    std::string deathMessage;

    void ReportDeath(const std::string& message)
    {
        {
            std::lock_guard<std::mutex> lock(deathMutex);
            deathMessage = message;
        }
        playerDied.store(true);
    }

    // World day-time, 0..23999 ticks (0/24000 = dawn, 6000 = noon, 12000 =
    // dusk, 18000 = midnight — vanilla's own convention). Set by NetworkClient
    // on PlayS2C::UpdateTime, read every frame by RenderThread to pick the
    // sky clear color between its night/day extremes. Defaults to 6000
    // (noon) so a server/dimension that never sends a clock update (this
    // client's own dev server never has so far) still renders full-bright
    // like before this existed, rather than snapping to some arbitrary time.
    std::atomic<int64_t> dayTimeTicks{6000};

    glm::vec3 cameraPosition{0.0f, 0.0f, 5.0f};

    // Set by VulkanInit's GLFW framebuffer-size callback (main thread) and
    // consumed by RenderThread (its own thread) to recreate the swapchain —
    // atomics so no lock is needed across that handoff.
    std::atomic<bool> framebufferResized{false};
    std::atomic<int> pendingFramebufferWidth{0};
    std::atomic<int> pendingFramebufferHeight{0};

    // Loads settings.json (next to the executable) and wires config up to
    // point at it. Must run before anything below reads a setting through
    // state->config — in particular before VulkanInit's Init(), which needs
    // Window.Width/Height.
    void LoadSettings()
    {
        configStorage.Load();
        config = &configStorage;

        resolution.x = static_cast<uint16_t>(std::get<uint32_t>(config->Get("Window.Width", uint32_t{854})));
        resolution.y = static_cast<uint16_t>(std::get<uint32_t>(config->Get("Window.Height", uint32_t{480})));
        targetFPS = static_cast<uint16_t>(std::get<uint32_t>(config->Get("Graphics.TargetFPS", uint32_t{60})));
    }

    // Drops every trace of the current world — chunks, both render lists,
    // tracked entities, and anything MeshingThread hasn't drained off
    // networkInbox yet (dropped rather than left to land afterward, since a
    // chunk from the world being left behind could otherwise bleed into the
    // one about to replace it) — then marks the world not-ready so TickLoop
    // holds off simulating again until a fresh spawn position and chunk
    // arrive. Used both when returning to the connect screen after a
    // disconnect (VolcanoClient.cpp's main loop) and, without reopening that
    // screen, when the server sends PlayS2C::Respawn mid-session — vanilla
    // sends that on every dimension change (not just death), and without
    // this the old dimension's chunks just kept rendering forever alongside
    // whatever streamed in for the new one ("ghost chunk geometry").
    //
    // Every container touched here already has its own mutex for exactly
    // this kind of cross-thread mutation (chunks stream in continuously
    // under the same locks), so this is safe to call from any thread — in
    // particular, directly from NetworkThread on a Respawn packet, not just
    // from the main thread the disconnect path happens to run on.
    //
    // Does NOT reopen the connect screen or touch the network connection
    // itself — callers that need that (the disconnect path) do it
    // separately, since a mid-session dimension change should stay connected.
    void ResetWorldState()
    {
        worldReady.store(false);
        world->Clear();
        {
            std::lock_guard<std::mutex> lock(renderListMutex);
            renderList.clear();
        }
        {
            std::lock_guard<std::mutex> lock(nonCubicRenderListMutex);
            nonCubicRenderList.clear();
        }
        {
            std::lock_guard<std::mutex> lock(entitiesMutex);
            entities.clear();
        }
        {
            std::lock_guard<std::mutex> lock(networkInbox.mutex);
            while (!networkInbox.chunks.empty()) networkInbox.chunks.pop();
            networkInbox.spawnPosition.reset();
        }
    }
};

}

#endif
