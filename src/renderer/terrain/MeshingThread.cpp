#include "MeshingThread.hpp"
#include "../../CrashReporter.hpp"
#include "ChunkMesher.hpp"
#include "LightEngine.hpp"
#include "models/ChunkSection.hpp"
#include "../misc/NonCubicMesher.hpp"
#include "Logger.hpp"
#include "TickLoop.hpp"
#include "ModLoader.hpp"
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <utility>

namespace Volcano {

namespace {

// Meshes one section (both the cube and non-cubic passes) and upserts the
// three render-list maps by SectionKey - shared by both the dirty-remesh
// path and the initial chunk-load path below, which differ only in how they
// get their Chunk& (already in World vs. not yet inserted) and whether they
// skip an empty section. Returns the two timings for the caller's own log
// line.
// `retry` means a slab allocation failed for at least one of the section's meshes: its
// old meshes were left in place, and the caller should remesh it again shortly.
struct SectionMeshTiming { double cubeMs; double nonCubicMs; bool retry; };

SectionMeshTiming MeshAndUpsertSection(GlobalState* state, const TextureManager* textureManager,
    const Chunk& chunk, const SectionKey& key)
{
    auto meshStart = std::chrono::steady_clock::now();
    Mesh mesh = ChunkMesher::MeshChunkSection(chunk, *state->world, *textureManager, key.sectionY);
    auto cubeDone = std::chrono::steady_clock::now();
    NonCubicMesher::ChunkMeshes nonCubicMeshes =
        NonCubicMesher::MeshChunkSection(chunk, *state->world, *textureManager, key.sectionY);
    mesh.fadeStart = chunk.GetFadeStart();
    nonCubicMeshes.cutout.fadeStart = chunk.GetFadeStart();
    nonCubicMeshes.translucent.fadeStart = chunk.GetFadeStart();
    auto meshDone = std::chrono::steady_clock::now();

    // A section that came back with nothing to draw (e.g. the last block in
    // it was just broken) gets its map entry erased instead of replaced -
    // otherwise a zero-count Mesh would sit in the render list forever
    // (harmless to draw, but never cleaned up, and would look like a stale
    // slab allocation if anyone went looking).
    //
    // Whatever mesh this replaces or erases hands its slab regions back
    // (SlabBuffer::Free delays actual reuse until no in-flight frame can
    // still be drawing it) - without this every remesh leaked its old
    // region and the slabs filled up from edits/light updates alone.
    auto upsert = [&](std::mutex& mutex, std::unordered_map<SectionKey, Mesh, SectionKeyHash>& list, const Mesh& m)
    {
        // Out of slab space: leave the section's current mesh drawing (stale but
        // present) rather than erasing it - the caller retries.
        if (m.allocationFailed) return;
        std::lock_guard<std::mutex> lock(mutex);
        auto existing = list.find(key);
        if (existing != list.end()) ReleaseMeshSlabs(existing->second);
        if (m.vertexCount > 0) list.insert_or_assign(key, m);
        else if (existing != list.end()) list.erase(existing);
        state->meshListGeneration++;
    };
    upsert(state->renderListMutex, state->renderList, mesh);
    upsert(state->nonCubicRenderListMutex, state->nonCubicRenderList, nonCubicMeshes.cutout);
    upsert(state->translucentRenderListMutex, state->translucentRenderList, nonCubicMeshes.translucent);

    const bool retry = mesh.allocationFailed || nonCubicMeshes.cutout.allocationFailed
        || nonCubicMeshes.translucent.allocationFailed;

    SectionMeshTiming timing{
        std::chrono::duration<double, std::milli>(cubeDone - meshStart).count(),
        std::chrono::duration<double, std::milli>(meshDone - cubeDone).count(),
        retry
    };

    // Smoothed for the debug HUD, which is the only reader - skipped while hidden.
    if (state->debugHudVisible)
    {
        float total = static_cast<float>(timing.cubeMs + timing.nonCubicMs);
        float previous = state->timings.meshMs.load();
        state->timings.meshMs.store(previous + (total - previous) * 0.05f);
    }
    return timing;
}

} // namespace

void ReleaseMeshSlabs(const Mesh& mesh)
{
    if (!ChunkMesher::ReleaseMesh(mesh)) NonCubicMesher::ReleaseMesh(mesh);
}

MeshingThread::MeshingThread(GlobalState* stateIn, const TextureManager* textureManagerIn)
    : state(stateIn), textureManager(textureManagerIn)
{
    // GlobalState::ResetWorldState can't include the meshers itself (NetDiag
    // builds GlobalState without them), so it calls through this hook.
    releaseMeshSlabsHook = &ReleaseMeshSlabs;
}

MeshingThread::~MeshingThread() = default;

void MeshingThread::Start()
{
    worker = std::jthread([this](std::stop_token st) { CrashReporter::InstallForThread(); ThreadEntry(st); });
}

void MeshingThread::RequestStop()
{
    worker.request_stop();
}

void MeshingThread::Stop()
{
    worker.request_stop();
    if (worker.joinable()) worker.join();
}

void MeshingThread::Pause()
{
    if (!worker.joinable()) return;
    pauseRequested.store(true);
    while (!paused.load() && worker.joinable() && !state->shouldClose)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void MeshingThread::Resume()
{
    pauseRequested.store(false);
    // Let the worker leave its parked state so an immediate Pause() waits properly.
    while (paused.load() && worker.joinable() && !state->shouldClose)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void MeshingThread::ThreadEntry(std::stop_token stopToken)
{
    Log::Info("Meshing thread created.");

    // Local to this connection attempt (mirrors what used to be
    // DrainNetworkInbox's static locals). Tracks which chunk the player is
    // actually spawning into, so worldReady only flips once *that* chunk has
    // been meshed and inserted - not just whichever chunk happens to arrive
    // first. The server has no obligation to send the spawn column first,
    // and gating on "any chunk" let the player fall through empty space
    // until it showed up.
    bool seenSpawn = false;
    std::optional<std::pair<int, int>> spawnChunk;

    // Sections whose last mesh couldn't get slab space (see SectionMeshTiming::
    // retry), with when to try again. A short delay lets frees land and keeps a
    // full slab from turning into a busy loop. Moved back into
    // NetworkInbox::dirtySections below once due.
    constexpr auto RETRY_DELAY = std::chrono::milliseconds(250);
    std::vector<std::pair<SectionKey, std::chrono::steady_clock::time_point>> retryQueue;

    // Neighbor sections queued for a seam remesh and not yet popped, so a burst
    // of chunk loads doesn't queue the same section repeatedly. Only touched
    // by this thread.
    std::unordered_set<SectionKey, SectionKeyHash> queuedSeamSections;

    while (!stopToken.stop_requested() && !state->shouldClose)
    {
        // Pause() handshake: parked here between jobs, never mid-section.
        if (pauseRequested.load())
        {
            paused.store(true);
            while (pauseRequested.load() && !stopToken.stop_requested() && !state->shouldClose)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            paused.store(false);
            continue;
        }

        std::unique_ptr<Chunk> chunk;
        std::optional<SectionKey> dirtySection;
        std::optional<glm::vec3> spawnPosition;
        size_t queueDepth = 0; // Remaining chunk-load backlog after this pop - see the timing log below.
        std::vector<std::pair<int, int>> pendingUnloads;
        std::vector<PendingBlockEdit> appliedEdits; // For BLOCK_CHANGED, fired once the inbox lock is dropped.

        {
            std::lock_guard<std::mutex> lock(state->networkInbox.mutex);

            // Apply every pending server Block Update up front, each turning
            // into more dirtySections entries the same way a local edit
            // does (see World::SetBlock) - these are rare/bursty (an
            // explosion, a correction to a mispredicted local break) rather
            // than a steady stream, so draining all of them here beats
            // leaving a correction sitting unapplied behind whatever else
            // is already queued. World's own mutex is separate from this
            // one and nothing holds both in the other order, so nesting the
            // SetBlock call inside this lock is safe.
            while (!state->networkInbox.pendingBlockEdits.empty())
            {
                PendingBlockEdit edit = state->networkInbox.pendingBlockEdits.front();
                state->networkInbox.pendingBlockEdits.pop_front();
                Block oldBlock = state->world->GetBlock(edit.position.x, edit.position.y, edit.position.z);
                if (state->modLoader != nullptr && state->modLoader->HasHandlers(EventType::BLOCK_CHANGED)) appliedEdits.push_back(edit);
                for (const SectionKey& key : state->world->SetBlock(edit.position, edit.block))
                {
                    state->networkInbox.dirtySections.push_back(key);
                }
                if (state->lightEngine != nullptr)
                {
                    for (const SectionKey& key : state->lightEngine->OnBlockChanged(edit.position, oldBlock, edit.block))
                    {
                        state->networkInbox.dirtySections.push_back(key);
                    }
                }
            }

            size_t lightUpdatesToProcess = state->networkInbox.pendingLightUpdates.size();
            std::vector<PendingLightUpdate> deferredLightUpdates;
            for (size_t i = 0; i < lightUpdatesToProcess; i++)
            {
                PendingLightUpdate update = std::move(state->networkInbox.pendingLightUpdates.front());
                state->networkInbox.pendingLightUpdates.pop_front();
                if (!state->world->HasChunk(update.chunkX, update.chunkZ))
                {
                    deferredLightUpdates.push_back(std::move(update));
                    continue;
                }
                for (const SectionKey& key : state->world->ApplyLightUpdate(update))
                {
                    state->networkInbox.dirtySections.push_back(key);
                }
            }
            for (PendingLightUpdate& update : deferredLightUpdates)
                state->networkInbox.pendingLightUpdates.push_back(std::move(update));

            while (!state->networkInbox.pendingUnloads.empty())
            {
                pendingUnloads.push_back(state->networkInbox.pendingUnloads.front());
                state->networkInbox.pendingUnloads.pop_front();
            }

            auto retryNow = std::chrono::steady_clock::now();
            for (auto it = retryQueue.begin(); it != retryQueue.end();)
            {
                if (it->second <= retryNow)
                {
                    state->networkInbox.dirtySections.push_back(it->first);
                    it = retryQueue.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            // Dirty sections (a local block edit, or a Block Update just
            // applied above) take priority over ordinary chunk-load
            // traffic, so an edit near the player doesn't sit behind a
            // backlog of newly-streaming chunks - see NetworkInbox::
            // dirtySections' own comment.
            if (!state->networkInbox.dirtySections.empty())
            {
                dirtySection = state->networkInbox.dirtySections.front();
                state->networkInbox.dirtySections.pop_front();
                queuedSeamSections.erase(*dirtySection);
            }
            else if (!state->networkInbox.chunks.empty())
            {
                chunk = std::move(state->networkInbox.chunks.front());
                state->networkInbox.chunks.pop();
            }
            queueDepth = state->networkInbox.chunks.size();
            if (state->debugHudVisible) state->timings.meshQueue.store(static_cast<uint32_t>(queueDepth));
            if (state->networkInbox.spawnPosition.has_value())
            {
                spawnPosition = state->networkInbox.spawnPosition;
                state->networkInbox.spawnPosition.reset();
            }
        }

        for (const PendingBlockEdit& edit : appliedEdits)
        {
            VC_EventData data{ sizeof(VC_EventData), nullptr, edit.position.x, edit.position.y, edit.position.z,
                static_cast<int32_t>(edit.block.visualId) };
            state->modLoader->Fire(EventType::BLOCK_CHANGED, &data);
        }

        // Server-side unloads (walked out of render distance): drop the chunk
        // from World and every section of it from the three render lists,
        // handing their slab regions back. Done here, on the only thread
        // that inserts chunks, so it's ordered against loads - see
        // NetworkInbox::pendingUnloads.
        for (const auto& [unloadX, unloadZ] : pendingUnloads)
        {
            state->world->RemoveChunk(unloadX, unloadZ);
            auto dropSections = [&](std::mutex& mutex, std::unordered_map<SectionKey, Mesh, SectionKeyHash>& list)
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (int sectionY = 0; sectionY < SECTIONS_PER_CHUNK; sectionY++)
                {
                    auto it = list.find(SectionKey{ unloadX, unloadZ, sectionY });
                    if (it == list.end()) continue;
                    ReleaseMeshSlabs(it->second);
                    list.erase(it);
                    state->meshListGeneration++;
                }
            };
            dropSections(state->renderListMutex, state->renderList);
            dropSections(state->nonCubicRenderListMutex, state->nonCubicRenderList);
            dropSections(state->translucentRenderListMutex, state->translucentRenderList);
        }

        if (spawnPosition.has_value())
        {
            if (!state->worldReady.load())
            {
                // Initial spawn. Safe against RenderThread's concurrent
                // Tick() calls only because worldReady is still false
                // here and these writes precede (in this thread's program
                // order) the eventual worldReady.store(true) below - see
                // that store's comment.
                state->player->SetPosition(*spawnPosition);
                state->tickLoop->SyncToPlayerPosition();
                spawnChunk = std::make_pair(
                    static_cast<int>(std::floor(spawnPosition->x / static_cast<float>(CHUNK_SIZE_X))),
                    static_cast<int>(std::floor(spawnPosition->z / static_cast<float>(CHUNK_SIZE_Z))));
                seenSpawn = true;
            }
            else
            {
                // Every later teleport: RenderThread owns TickLoop's
                // simulation state once worldReady is set, so hand it over
                // rather than writing it from here.
                state->tickLoop->QueueTeleport(*spawnPosition);
            }
        }

        if (dirtySection.has_value())
        {
            // The chunk already lives in World (it was fully loaded before
            // any edit could target it) - nothing to insert, just re-mesh
            // and upsert the one section. If the chunk unloaded/was never
            // there (a stale request racing a disconnect), there's nothing
            // to do.
            const Chunk* dirtyChunk = state->world->GetChunk(dirtySection->chunkX, dirtySection->chunkZ);
            if (dirtyChunk != nullptr)
            {
                SectionMeshTiming timing = MeshAndUpsertSection(state, textureManager, *dirtyChunk, *dirtySection);
                if (timing.retry) retryQueue.emplace_back(*dirtySection, std::chrono::steady_clock::now() + RETRY_DELAY);
                if (Log::MeshEventsEnabled())
                {
                    Log::Info("[MESH] section (" + std::to_string(dirtySection->chunkX) + ","
                        + std::to_string(dirtySection->chunkZ) + "," + std::to_string(dirtySection->sectionY) + "): "
                        + std::to_string(timing.cubeMs) + "ms cube + " + std::to_string(timing.nonCubicMs) + "ms non-cube");
                }
            }
        }
        else if (chunk)
        {
            int chunkX = chunk->getX();
            int chunkZ = chunk->getZ();
            chunk->SetFadeStart(std::chrono::steady_clock::now());

            // Meshed section-by-section before InsertChunk below places this
            // chunk itself into World, but that's fine - ChunkMesher only
            // needs World for this chunk's neighbors (see its own comment),
            // not itself. Sections with nothing to draw (common near
            // build-height limits) are skipped entirely rather than
            // producing (and slab-allocating) an empty mesh for them.
            double totalCubeMs = 0.0, totalNonCubicMs = 0.0;
            int meshedSections = 0;
            for (int sectionY = 0; sectionY < SECTIONS_PER_CHUNK; sectionY++)
            {
                if (chunk->IsSectionEmpty(sectionY)) continue;

                SectionMeshTiming timing = MeshAndUpsertSection(state, textureManager, *chunk, { chunkX, chunkZ, sectionY });
                if (timing.retry)
                {
                    // The chunk is inserted into World just below, so the retry
                    // finds it there.
                    retryQueue.emplace_back(SectionKey{ chunkX, chunkZ, sectionY }, std::chrono::steady_clock::now() + RETRY_DELAY);
                }
                totalCubeMs += timing.cubeMs;
                totalNonCubicMs += timing.nonCubicMs;
                meshedSections++;
            }
            state->world->InsertChunk(chunkX, chunkZ, std::move(*chunk));

            // Loaded neighbors meshed this border as air; remesh them now that
            // the chunk exists. Edges and diagonals both, since fluid corner
            // heights read diagonals.
            {
                std::lock_guard<std::mutex> lock(state->networkInbox.mutex);
                for (int dz = -1; dz <= 1; dz++)
                {
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        if (dx == 0 && dz == 0) continue;
                        const Chunk* neighbor = state->world->GetChunk(chunkX + dx, chunkZ + dz);
                        if (neighbor == nullptr) continue;
                        for (int sectionY = 0; sectionY < SECTIONS_PER_CHUNK; sectionY++)
                        {
                            if (neighbor->IsSectionEmpty(sectionY)) continue;
                            SectionKey key{ chunkX + dx, chunkZ + dz, sectionY };
                            if (queuedSeamSections.insert(key).second) state->networkInbox.dirtySections.push_back(key);
                        }
                    }
                }
            }

            // Diagnostic timing - see whether a slow-to-appear chunk is
            // actually expensive to mesh, or just backed up behind
            // queueDepth others in this single-threaded queue (see this
            // loop's own comment on why there's no parallelism here yet).
            if (Log::MeshEventsEnabled())
            {
                Log::Info("[MESH] chunk (" + std::to_string(chunkX) + "," + std::to_string(chunkZ) + "): "
                    + std::to_string(totalCubeMs) + "ms cube + " + std::to_string(totalNonCubicMs) + "ms non-cube, "
                    + std::to_string(meshedSections) + "/" + std::to_string(SECTIONS_PER_CHUNK) + " sections, "
                    + std::to_string(queueDepth) + " queued");
            }

            // worldReady must be the last write of this transition - see
            // its comment in GlobalState.hpp for why RenderThread relies on
            // that order.
            if (seenSpawn && spawnChunk.has_value()
                && spawnChunk->first == chunkX && spawnChunk->second == chunkZ
                && !state->worldReady.load())
            {
                state->worldReady.store(true);
            }
        }

        if (dirtySection.has_value() || chunk)
        {
            // Checkpoint after every chunk/section, not just once the queue
            // empties - a big backlog can otherwise hog a core long enough
            // to starve RenderThread, which now drives physics too.
            std::this_thread::yield();
        }
        else if (!spawnPosition.has_value())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

} // namespace Volcano
