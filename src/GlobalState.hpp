#pragma once
#include "models/GUIWindow.hpp"
#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include "renderer/models/Mesh.hpp"
#include "renderer/entity/models/Entity.hpp"
#include "renderer/entity/models/PlayerEntity.hpp"
#include "renderer/gui/models/GUIWindow.hpp"
#include "renderer/terrain/models/Chunk.hpp"
#include "renderer/terrain/models/ChunkSection.hpp"
#include "renderer/terrain/models/World.hpp"
#include "InputHandler.hpp"
#include "Player.hpp"
#include "PlayerAttributes.hpp"
#include "Config.hpp"
#include "SystemInfo.hpp"
#include "inventory/InventoryManager.hpp"
#include "network/CommandTree.hpp"

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
// around) since it needs the full GlobalState - a forward declaration is
// enough here for a pointer member.
class TickLoop;

// Defined in network/Connection.hpp. Forward-declared here (rather than
// including that header, which would pull asio into every GlobalState.hpp
// includer) purely so GlobalState::activeConnection below can hold a
// pointer to it.
class Connection;

// Defined in interaction/InteractionManager.hpp, which includes this header
// (not the other way around) since it needs the full GlobalState
class InteractionManager;

class AudioManager;
class MusicController;
class ModLoader;
class SoundRegistry;
class BlockSounds;
class EntitySounds;
class CombatSounds;
class RideController;
class LightEngine;
class ItemRenderer;
class ParticleSystem;
class ResourceManager;

// Defined in renderer/engine/RenderEngine.hpp - the abstraction layer every
// non-renderer system talks to instead of Vulkan. Forward-declared for the
// same reason as TickLoop above.
namespace Engine {
class RenderEngine;
}

struct Resolution {
    uint16_t x;
    uint16_t y;
};

// One clientbound Block Update (PlayS2C::BlockUpdate) waiting to be applied
// - a world position plus the already-resolved Block (visualId/
// nonCubeVisualId/collisionShapeId), since the network thread never touches
// World directly (see NetworkInbox's own comment) but resolving a protocol
// state id via BlockRegistry is cheap/thread-safe to do up front anyway.
struct PendingBlockEdit {
    glm::ivec3 position; // World-block coordinates.
    Block block;
};

// Handoff point from the network thread to the main thread. The network
// thread parses packets into plain-data Chunks and never touches World or
// renderList directly
struct NetworkInbox {
    std::mutex mutex;
    std::queue<std::unique_ptr<Chunk>> chunks;
    // Carries the initial spawn AND every later teleport the server sends
    // (Player Position, 0x48). Latest wins if two land before MeshingThread
    // drains it
    std::optional<glm::vec3> spawnPosition;

    // Sections needing a remesh after a local block edit (see World::
    // SetBlock/BreakBlockEvent) - checked by MeshingThread ahead of `chunks`
    // above, so an edit near the player doesn't sit behind a backlog of
    // ordinary chunk-load traffic. Guarded by the same `mutex`.
    std::deque<SectionKey> dirtySections;

    // Block Update packets waiting to be applied to World (see
    // PendingBlockEdit above) - drained by MeshingThread before it even
    // looks at dirtySections, converting each into World::SetBlock calls
    // whose own returned SectionKeys then feed dirtySections the usual way.
    // This is what corrects a locally-mispredicted BreakBlockEvent edit
    // back to the server's real state (see its own comment)
    std::deque<PendingBlockEdit> pendingBlockEdits;

    // Update Light packets waiting to be applied
    std::deque<PendingLightUpdate> pendingLightUpdates;

    // Unload Chunk packets (chunkX, chunkZ)
    std::deque<std::pair<int, int>> pendingUnloads;
};

// Where the client is in getting into a world
enum class ConnectionPhase : uint8_t {
    Idle,           // On a menu; no connection attempt in progress.
    StartingServer, // Singleplayer: server.jar launching, not accepting connections yet.
    Connecting,     // TCP connect + Handshake + Login.
    Configuring,    // Configuration state (registries, tags, known packs, ...).
    LoadingTerrain, // Play state entered; waiting for the spawn chunk (worldReady).
    Playing,        // worldReady has been reached at least once this session.
    Disconnecting,  // Player-requested leave; waiting for the network thread (and local server) to wind down.
};

// Handoff point for changes to the local player's own velocity that don't
// originate in TickLoop itself: server knockback (PlayS2C::SetEntityMotion
// for our own entity id), explosion pushes (PlayS2C::Explosion), and the
// sprint-hit slowdown InteractionManager applies to us when we land a
// knockback hit on another player. TickLoop owns velocity/sprinting on
// NetworkThread, while these arrive on NetworkThread (packets) and the
// render thread (attacks) - so they're queued here instead, and TickLoop::
// Tick drains them in arrival order before its next fixed step. Order
// matters: vanilla applies each one immediately as it happens, so a
// knockback packet landing right after our own attack must overwrite the
// attack's 0.6x slowdown, not be scaled by it.
struct LocalMotionEvent {
    enum class Kind : uint8_t {
        SetVelocity,     // Replace velocity outright (vanilla's lerpMotion/setDeltaMovement).
        AddVelocity,     // Add onto current velocity (vanilla's addDeltaMovement).
        AttackSlowdown,  // Scale horizontal velocity by `vector.x` and stop sprinting.
    };
    Kind kind;
    glm::vec3 vector{0.0f};
};

struct LocalMotionInbox {
    std::mutex mutex;
    std::vector<LocalMotionEvent> events;

    void Push(LocalMotionEvent event)
    {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(event);
    }
};

// Set by MeshingThread to ReleaseMeshSlabs, so ResetWorldState can hand
// cleared meshes' slab regions back without GlobalState depending on the
// meshers (NetDiag builds this header without them - null there).
inline void (*releaseMeshSlabsHook)(const Mesh&) = nullptr;

struct GlobalState {
    // Owns the client's settings
    Config configStorage;
    Config* config{nullptr};

    InputHandler* input;
    Player* player;
    GLFWwindow* window;
    World* world;
    PlayerAttributes* attributes;
    TickLoop* tickLoop;
    InteractionManager* interaction;
    AudioManager* audioManager{nullptr};
    MusicController* musicController{nullptr};
    ModLoader* modLoader{nullptr}; // Owned by main(); fires events to loaded mods.
    SoundRegistry* soundRegistry{nullptr};
    LightEngine* lightEngine{nullptr}; // Local light for block edits; the server's Update Light corrects it.
    EntitySounds* entitySounds{nullptr}; // Hurt sounds; called from the network thread.
    CombatSounds* combatSounds{nullptr}; // Hit, spear, mace and arrow sounds; called from the main and network threads.
    BlockSounds* blockSounds{nullptr}; // Step, break, place and mining-hit sounds; called from the tick and main threads.
    ResourceManager* resourceManager{nullptr}; // Layered resource packs; created in main() after LoadSettings().
    ItemRenderer* itemRenderer{nullptr}; // 3D item models (GUI icons now; hand and dropped items later).

    // The client's particles. Spawn() may be called from any thread (the network thread does, for the Level Particles
    // packet); everything else is the render thread's. See ParticlePass.
    ParticleSystem* particles{nullptr};

    // The rendering engine (VulkanRenderEngine today). Owned by main();
    // set before its Init() runs. Use it to add/remove render passes, switch
    // shader packs (SetShaderPack) or reload shaders
    Engine::RenderEngine* renderEngine{nullptr};

    // CPU/GPU/driver/API info for the debug overlay
    SystemInfo systemInfo;

    // Vanilla's "attack strength" ticker: counts UP once per fixed tick
    // (TickLoop::FixedStep) since the last swing, instead of a cooldown that
    // counts down. InteractionManager::GetAttackStrengthScale divides this
    // by a ticks-to-full-charge figure it recomputes from the live
    // generic.attack_speed attribute every call rather than caching it at
    // swing time
    std::atomic<uint32_t> attackStrengthTicker{0};

    // When the local player last swung an arm (steady_clock ns; 0 = never).
    // Written by SendArmAnimation on the render thread; read by HandModel.
    std::atomic<int64_t> localSwingNs{0};

    // Tick counters for things that update "per tick" (shadow recompute): the
    // client's fixed 20 Hz TickLoop step, and the server's
    std::atomic<uint64_t> clientTicks{0};
    std::atomic<uint64_t> serverTicks{0};

    // 0..1 through the local player's current arm swing, or 0 when idle.
    // While a block is being mined the swing repeats until it finishes.
    float GetLocalSwingProgress() const
    {
        constexpr float SWING_SECONDS = 0.3f; // Vanilla's 6-tick arm swing.
        int64_t startNs = localSwingNs.load(std::memory_order_relaxed);
        if (startNs == 0) return 0.0f;
        int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        float seconds = static_cast<float>(nowNs - startNs) * 1e-9f;
        if (seconds < 0.0f) return 0.0f;
        if (seconds >= SWING_SECONDS - 0.1f && miningTicksRequired.load(std::memory_order_relaxed) > 0)
        {
            seconds = std::fmod(seconds, SWING_SECONDS);
        }
        return seconds < SWING_SECONDS ? seconds / SWING_SECONDS : 0.0f;
    }

    // True while a continuous item use (eating, blocking, a drawn bow) is in
    // progress. Written by the render thread (InteractionManager); read by
    // the tick thread, which slows movement and stops sprinting. usingItemAction
    // is the UseAction ordinal (inventory/ItemBehavior.hpp); useStartNs is
    // steady_clock ns.
    std::atomic<bool> usingItem{false};
    std::atomic<uint8_t> usingItemAction{0};
    std::atomic<int64_t> useStartNs{0};

    // Active block-break, published for the tick thread's crosshair
    // indicator update. Written by the render thread (InteractionManager
    // arms/clears mining); read by the tick thread (TickLoop::FixedStep).
    // miningTicksRequired 0 = not mining; miningStartNs is steady_clock ns.
    std::atomic<int32_t> miningTicksRequired{0};
    std::atomic<int64_t> miningStartNs{0};

    // Crosshair indicator fill (0..1, or < 0 = hidden): block-break progress
    // while mining, else attack strength while recharging. Written by the
    // tick thread once per tick (previous <- current, current <- new); read
    // by the render thread, which lerps them by TickLoop::GetPartialTick().
    std::atomic<float> crosshairProgressPrevious{-1.0f};
    std::atomic<float> crosshairProgressCurrent{-1.0f};

    // Client-generated sequence number for the dig/place/use-item packets
    // (PlayerAction, BlockPlace, UseItem)
    std::atomic<int32_t> interactionSequence{0};

    // Local player's current gamemode, protocol's own mapper values (0 =
    // survival, 1 = creative, 2 = adventure, 3 = spectator - see PlayS2C::
    // LoginPlay's SpawnInfo.gamemode and PlayS2C::GameStateChange). Informational
    // only right now
    std::atomic<uint8_t> localGameMode{0};

    // Name of the dimension the local player is in (e.g. "minecraft:the_nether").
    // Written by the network thread on LoginPlay/Respawn, read by MusicController.
    std::string currentDimension;
    std::mutex currentDimensionMutex;

    // Whether the local player can insta-mine
    std::atomic<bool> canInstabuild{false};

    // Player inventory (hotbar/main inventory/armor/offhand slot groups) -
    // held directly rather than behind a pointer, same as networkInbox below,
    // since there's exactly one per session. Not yet populated by anything:
    // no packet handling for Window Items/Set Container Slot exists yet, so
    // this is currently just the storage those will write into and the
    // future inventory/hotbar/armor GUI will read from
    InventoryManager inventory;

    // Current state:
    bool shouldClose{false};
    Resolution resolution;
    uint16_t targetFPS{60};
    uint16_t currentFPS{0};

    // The swapchain's actual active VkPresentModeKHR, cast down to a byte -
    // fits every mode this client can request (FIFO=2/IMMEDIATE=0, per
    // Graphics.VSync
    uint8_t presentMode{0};
    // Keyed by SectionKey (chunk column + 16-tall Y section - see
    // ChunkSection.hpp) rather than an append-only list, since a section
    // can now be remeshed in place after a block edit (World::SetBlock):
    // MeshingThread upserts by key instead of appending a duplicate, and
    // erases the key entirely if a remesh comes back empty (e.g. the last
    // block in a section was broken). Same entities/entitiesMutex-style
    // map+mutex convention as elsewhere in this file.
    //
    // Guards renderList itself (not the Meshes' Vulkan handles, which are
    // never mutated after being placed in the slab buffers) - MeshingThread
    // writes to it as chunks stream in or a section gets remeshed, while
    // RenderThread iterates it every frame on its own thread. Without this,
    // an insert that rehashes while the render thread is mid-iteration is a
    // use-after-free with no exception and no log line, just a sudden
    // crash.
    std::unordered_map<SectionKey, Mesh, SectionKeyHash> renderList = {};
    std::mutex renderListMutex;

    // Bumped whenever a section is added, replaced or removed in ANY of the
    // three render lists (renderList, nonCubicRenderList, translucentRenderList),
    // so the passes' cached visible sets (see ChunkMeshPass::GetVisible) know
    // to recompute. Coarse on purpose: one counter for all three lists.
    std::atomic<uint64_t> meshListGeneration{0};

    // Debug HUD (F3). Off by default; every piece of debug data below (and
    // the GPU timestamp queries, occlusion stats, ...) is only collected
    // while this is true, so it costs nothing when hidden.
    std::atomic<bool> debugHudVisible{false};

    // Triangles actually drawn this frame, for the debug HUD (GUIController),
    // split by what drew them
    enum TriangleCategory { TRIANGLES_TERRAIN, TRIANGLES_CUTOUT, TRIANGLES_TRANSLUCENT, TRIANGLES_ENTITY, TRIANGLE_CATEGORY_COUNT };
    uint64_t trianglesDrawn[TRIANGLE_CATEGORY_COUNT] = {};

    // Last frame's in-frustum section draws and how many Hi-Z occluded
    // (terrain + non-cubic lists). Same ownership and gating as trianglesDrawn.
    uint32_t sectionsTested = 0, sectionsOccluded = 0;

    // CPU time the chunk passes (terrain + cutout + water) spend per frame on
    // culling, back-to-front sorting and recording draws, for the debug HUD.
    // chunkPhaseFrameMs is summed during RecordPasses and folded into the
    // smoothed chunkPhaseMs after it. Same ownership and gating as above.
    enum ChunkPhase { CHUNK_PHASE_CULL, CHUNK_PHASE_SORT, CHUNK_PHASE_DRAW, CHUNK_PHASE_COUNT };
    float chunkPhaseFrameMs[CHUNK_PHASE_COUNT] = {};
    float chunkPhaseMs[CHUNK_PHASE_COUNT] = {};

    // Smoothed per-stage timings in milliseconds, for the debug HUD. Each
    // field has exactly one writer thread, so plain atomic stores suffice.
    struct Timings
    {
        std::atomic<float> renderCpuMs{0.0f};  // RenderThread: frame work, excluding pacing and GPU waits
        std::atomic<float> gpuWaitMs{0.0f};    // RenderThread: blocked on the fence / swapchain acquire
        std::atomic<float> gpuMs{0.0f};        // GPU: command buffer start to end (timestamp queries); < 0 = unsupported
        std::atomic<float> meshMs{0.0f};       // MeshingThread: per section (cube + non-cube)
        std::atomic<uint32_t> meshQueue{0};    // MeshingThread: chunks still waiting to be meshed
        std::atomic<float> waitRawMs{0.0f};    // Engine -> RenderThread: this frame's wait, unsmoothed
    } timings;

    // Non-cubic meshes (transparent full cubes, partial-volume shapes, and
    // cross-shaped plants
    std::unordered_map<SectionKey, Mesh, SectionKeyHash> nonCubicRenderList = {};
    std::mutex nonCubicRenderListMutex;

    // Genuinely translucent meshes (currently water only - see
    // NonCubicMesher::ChunkMeshes::translucent) for RenderThread's separate
    // translucent pass: alpha blended like nonCubicRenderList's cutout pass,
    // but with depth WRITE off (VulkanInit's translucentPipeline) so two
    // overlapping translucent surfaces actually blend instead of whichever
    // one happened to draw first winning the depth test outright. Same
    // population/locking/keying story as nonCubicRenderList
    std::unordered_map<SectionKey, Mesh, SectionKeyHash> translucentRenderList = {};
    std::mutex translucentRenderListMutex;

    // Entities tracked for EntityRenderer's batched draw pass, keyed by
    // network entity ID for O(1) update/remove on every Move/Teleport/
    // Remove Entities packet. Populated directly by NetworkClient's Play
    // packet loop (Entity has no Vulkan handles, unlike Mesh/renderList, so
    // there's no need to hand it off through a main-thread-only inbox first)
    // - guarded by entitiesMutex the same way renderList is guarded against
    // RenderThread, since NetworkClient runs on its own thread too.
    std::unordered_map<uint32_t, Entity> entities = {};
    std::mutex entitiesMutex;

    // Other players, keyed by network entity ID like `entities` above but
    // kept in their own map so their player-only fields (uuid, username,
    // equipment, health, pose flags
    std::unordered_map<uint32_t, PlayerEntity> playerEntities = {};

    // Looks an entity id up in both entities and playerEntities. Caller must
    // hold entitiesMutex. The returned pointer is only valid while it does.
    Entity* FindEntity(uint32_t id)
    {
        if (auto it = entities.find(id); it != entities.end()) return &it->second;
        if (auto it = playerEntities.find(id); it != playerEntities.end()) return &it->second;
        return nullptr;
    }

    PlayerEntity* FindPlayerEntity(uint32_t id)
    {
        auto it = playerEntities.find(id);
        return it != playerEntities.end() ? &it->second : nullptr;
    }

    // Calls fn(id, const Entity&) for every tracked entity, players
    // included, for code that only needs the common Entity fields. Caller
    // must hold entitiesMutex.
    template <typename Fn>
    void ForEachEntity(Fn&& fn) const
    {
        for (const auto& [id, entity] : entities) fn(id, entity);
        for (const auto& [id, player] : playerEntities) fn(id, static_cast<const Entity&>(player));
    }

    // Our own network entity id, from PlayS2C::LoginPlay
    std::atomic<int32_t> localEntityId{-1};

    // Network id of the vehicle or mount we are riding, -1 when on foot.
    // Written by NetworkThread (Set Passengers, Remove Entities, world reset),
    // read by RenderThread (TickLoop, RideController) and TickThread.
    std::atomic<int32_t> ridingEntityId{-1};

    // Steers the ridden vehicle; owned by main, see RideController.hpp.
    RideController* rideController = nullptr;

    // Our own username, as confirmed by Login Success. Written by
    // NetworkThread, read by the render thread for the third-person nametag.
    std::string localUsername;
    std::mutex localUsernameMutex;

    // See LocalMotionInbox's own comment.
    LocalMotionInbox localMotion;

    // Whether the crosshair is currently over an attackable entity
    std::atomic<bool> lookingAtEntity{false};

    // Tab-list names, keyed by hex-formatted UUID (see NetworkClient's
    // FormatUuid) rather than the raw 16-byte array - same
    // write-from-network/read-from-render split and map+mutex convention as
    // entities/entitiesMutex above. Populated by the Player Info Update/
    // Remove packets in NetworkClient::RunPlayLoop.
    std::unordered_map<std::string, std::string> playerList = {};
    std::mutex playerListMutex;

    // Each tab-list player's game mode (Player Info Update's
    // UPDATE_GAME_MODE), keyed the same way as playerList and guarded by the
    // same mutex. Kept apart from the PlayerEntity itself because the info
    // update usually arrives before that player's Spawn Entity does -
    // NetworkClient copies it onto the PlayerEntity at spawn, and onto an
    // already-spawned one when it changes.
    std::unordered_map<std::string, GameMode> playerGameModes = {};

    // The server's command tree from the Commands packet. The network thread
    // replaces it whole whenever one arrives; readers copy the pointer under
    // the lock and walk their copy, which never changes. Null until the first.
    std::shared_ptr<const CommandTree> commands;
    std::mutex commandsMutex;

    // Last values reported by the server's Set Health packet (see
    // NetworkClient's PlayS2C::SetHealth handler) - read every frame by
    // GUIController::RenderPlayerStatusBars on the render thread, written
    // by the network thread the same way entities/entitiesMutex above is.
    // Vanilla's own ranges (health 0-20, food 0-20, saturation 0-5, though
    // saturation can technically exceed 5) aren't enforced here; this just
    // stores whatever the server actually sends.
    float health{20.0f};
    int32_t food{20};
    float saturation{5.0f};
    std::mutex healthMutex;

    // Last Set Experience values: progress through the current level (0-1)
    // and the level itself. Written by the network thread, read by
    // GUIController::RenderExperienceBar.
    float experienceProgress{0.0f};
    int32_t experienceLevel{0};
    std::mutex experienceMutex;

    NetworkInbox networkInbox;

    // Recent lines from a starting/running singleplayer server's stdout and
    // stderr (see LocalServer::onOutputLine, wired up by NetworkThread) -
    // capped so a chatty server can't grow this unboundedly. Read every
    // frame by the starting-server screen's customContent (render thread),
    // written by NetworkThread's background thread; cleared at the start of
    // each singleplayer connection attempt.
    std::mutex serverLogMutex;
    std::deque<std::string> serverLogLines;

    // The live Play-session connection, published by NetworkThread right
    // before it starts pumping the Play packet loop and cleared again once
    // that loop returns
    std::atomic<std::shared_ptr<Connection>> activeConnection{};

    // Set once by MeshingThread, after it has synced TickLoop's cached
    // position and inserted the specific chunk the player is spawning into
    // (not just any chunk - the server has no obligation to send that one
    // first); read every call by TickLoop::Tick (RenderThread) to hold
    // off gravity/collision until there's an actual floor to land on -
    // otherwise the player free-falls through the still-empty world from the
    // moment the app starts, before any chunk has loaded. Must be set only
    // after all of MeshingThread's other writes for this transition, since
    // RenderThread treats seeing it become true as permission to start
    // reading/writing TickLoop's position state, and TickThread treats it as
    // permission to send PlayC2S::PlayerLoaded and start queueing position
    // reports (see TickThread's own comment).
    std::atomic<bool> worldReady{false};

    // See ConnectionPhase. connectionDetail is the human-readable specifics
    // for the current phase (the host being connected to, the Configuration
    // packet currently being processed) - guarded by connectionDetailMutex
    // since std::string isn't safe to share via the atomic alone.
    std::atomic<ConnectionPhase> connectionPhase{ConnectionPhase::Idle};
    std::mutex connectionDetailMutex;
    std::string connectionDetail;

    void SetConnectionPhase(ConnectionPhase phase, const std::string& detail = "")
    {
        {
            std::lock_guard<std::mutex> lock(connectionDetailMutex);
            connectionDetail = detail;
        }
        connectionPhase.store(phase);
    }

    // Updates just the detail line, leaving the phase alone
    void SetConnectionDetail(const std::string& detail)
    {
        std::lock_guard<std::mutex> lock(connectionDetailMutex);
        connectionDetail = detail;
    }

    std::string GetConnectionDetail()
    {
        std::lock_guard<std::mutex> lock(connectionDetailMutex);
        return connectionDetail;
    }

    // GLFW only guarantees glfwSetInputMode/glfwGetWindowAttrib are safe to
    // call from the thread that created the window
    std::atomic<int> pendingCursorMode{-1};
    std::atomic<bool> windowFocused{false};

    // Set by NetworkThread when a session ends unexpectedly (kicked,
    // connection dropped, login failure) rather than via a deliberate
    // app-level stop
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

    // Set by NetworkClient on PlayS2C::PlayerCombatKill
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
    // dusk, 18000 = midnight - vanilla's own convention). Set by NetworkClient
    // on PlayS2C::UpdateTime, read every frame by RenderThread to pick the
    // sky clear color between its night/day extremes. Defaults to 6000
    // (noon) so a server/dimension that never sends a clock update (this
    // client's own dev server never has so far) still renders full-bright
    // like before this existed, rather than snapping to some arbitrary time.
    std::atomic<int64_t> dayTimeTicks{6000};

    glm::vec3 cameraPosition{0.0f, 0.0f, 5.0f};

    // Set by VulkanInit's GLFW framebuffer-size callback (main thread) and
    // consumed by RenderThread (its own thread) to recreate the swapchain -
    // atomics so no lock is needed across that handoff.
    std::atomic<bool> framebufferResized{false};
    std::atomic<int> pendingFramebufferWidth{0};
    std::atomic<int> pendingFramebufferHeight{0};

    // Loads settings.json (next to the executable) and wires config up to
    // point at it. Must run before anything below reads a setting through
    // state->config - in particular before VulkanInit's Init(), which needs
    // Window.Width/Height.
    void LoadSettings()
    {
        configStorage.Load();
        config = &configStorage;

        resolution.x = static_cast<uint16_t>(std::get<uint32_t>(config->Get("Window.Width", uint32_t{854})));
        resolution.y = static_cast<uint16_t>(std::get<uint32_t>(config->Get("Window.Height", uint32_t{480})));
        targetFPS = static_cast<uint16_t>(std::get<uint32_t>(config->Get("Graphics.TargetFPS", uint32_t{60})));
    }

    // Drops every trace of the current world
    void ResetWorldState()
    {
        worldReady.store(false);
        ridingEntityId.store(-1);
        world->Clear();
        auto clearList = [](std::mutex& mutex, std::unordered_map<SectionKey, Mesh, SectionKeyHash>& list)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (releaseMeshSlabsHook)
            {
                for (const auto& [key, mesh] : list) releaseMeshSlabsHook(mesh);
            }
            list.clear();
        };
        clearList(renderListMutex, renderList);
        clearList(nonCubicRenderListMutex, nonCubicRenderList);
        clearList(translucentRenderListMutex, translucentRenderList);
        meshListGeneration++;
        {
            std::lock_guard<std::mutex> lock(entitiesMutex);
            entities.clear();
            playerEntities.clear();
        }
        {
            std::lock_guard<std::mutex> lock(localMotion.mutex);
            localMotion.events.clear();
        }
        {
            std::lock_guard<std::mutex> lock(networkInbox.mutex);
            while (!networkInbox.chunks.empty()) networkInbox.chunks.pop();
            networkInbox.spawnPosition.reset();
            networkInbox.dirtySections.clear();
            networkInbox.pendingBlockEdits.clear();
            networkInbox.pendingLightUpdates.clear();
            networkInbox.pendingUnloads.clear();
        }
    }
};

}

#endif
