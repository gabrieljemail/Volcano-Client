#include "MeshingThread.hpp"
#include "ChunkMesher.hpp"
#include "models/ChunkSection.hpp"
#include "../misc/NonCubicMesher.hpp"
#include "Logger.hpp"
#include "TickLoop.hpp"
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

namespace Volcano {

namespace {

// Meshes one section (both the cube and non-cubic passes) and upserts the
// three render-list maps by SectionKey — shared by both the dirty-remesh
// path and the initial chunk-load path below, which differ only in how they
// get their Chunk& (already in World vs. not yet inserted) and whether they
// skip an empty section. Returns the two timings for the caller's own log
// line.
struct SectionMeshTiming { double cubeMs; double nonCubicMs; };

SectionMeshTiming MeshAndUpsertSection(GlobalState* state, const TextureManager* textureManager,
    const Chunk& chunk, const SectionKey& key)
{
    auto meshStart = std::chrono::steady_clock::now();
    Mesh mesh = ChunkMesher::MeshChunkSection(chunk, *state->world, *textureManager, key.sectionY);
    auto cubeDone = std::chrono::steady_clock::now();
    NonCubicMesher::ChunkMeshes nonCubicMeshes =
        NonCubicMesher::MeshChunkSection(chunk, *state->world, *textureManager, key.sectionY);
    auto meshDone = std::chrono::steady_clock::now();

    // A section that came back with nothing to draw (e.g. the last block in
    // it was just broken) gets its map entry erased instead of replaced —
    // otherwise a zero-count Mesh would sit in the render list forever
    // (harmless to draw, but never cleaned up, and would look like a stale
    // slab allocation if anyone went looking).
    auto upsert = [&](std::mutex& mutex, std::unordered_map<SectionKey, Mesh, SectionKeyHash>& list, const Mesh& m)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (m.vertexCount > 0) list.insert_or_assign(key, m);
        else list.erase(key);
    };
    upsert(state->renderListMutex, state->renderList, mesh);
    upsert(state->nonCubicRenderListMutex, state->nonCubicRenderList, nonCubicMeshes.cutout);
    upsert(state->translucentRenderListMutex, state->translucentRenderList, nonCubicMeshes.translucent);

    // A slab-allocation failure (see ChunkMesher/NonCubicMesher's own
    // Log::Error at the Allocate() call site) already logged which section
    // it happened to and is otherwise indistinguishable here from a
    // genuinely empty one (both end up with vertexCount 0) — nothing more
    // to do with it at this level.

    return {
        std::chrono::duration<double, std::milli>(cubeDone - meshStart).count(),
        std::chrono::duration<double, std::milli>(meshDone - cubeDone).count()
    };
}

} // namespace

MeshingThread::MeshingThread(GlobalState* stateIn, const TextureManager* textureManagerIn)
    : state(stateIn), textureManager(textureManagerIn)
{}

MeshingThread::~MeshingThread() = default;

void MeshingThread::Start()
{
    worker = std::jthread([this](std::stop_token st) { ThreadEntry(st); });
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

void MeshingThread::ThreadEntry(std::stop_token stopToken)
{
    Log::Info("Meshing thread created.");

    // Local to this connection attempt (mirrors what used to be
    // DrainNetworkInbox's static locals). Tracks which chunk the player is
    // actually spawning into, so worldReady only flips once *that* chunk has
    // been meshed and inserted — not just whichever chunk happens to arrive
    // first. The server has no obligation to send the spawn column first,
    // and gating on "any chunk" let the player fall through empty space
    // until it showed up.
    bool seenSpawn = false;
    std::optional<std::pair<int, int>> spawnChunk;

    while (!stopToken.stop_requested() && !state->shouldClose)
    {
        std::unique_ptr<Chunk> chunk;
        std::optional<SectionKey> dirtySection;
        std::optional<glm::vec3> spawnPosition;
        size_t queueDepth = 0; // Remaining chunk-load backlog after this pop — see the timing log below.

        {
            std::lock_guard<std::mutex> lock(state->networkInbox.mutex);

            // Apply every pending server Block Update up front, each turning
            // into more dirtySections entries the same way a local edit
            // does (see World::SetBlock) — these are rare/bursty (an
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
                for (const SectionKey& key : state->world->SetBlock(edit.position, edit.block))
                {
                    state->networkInbox.dirtySections.push_back(key);
                }
            }

            while (!state->networkInbox.pendingLightUpdates.empty())
            {
                PendingLightUpdate update = std::move(state->networkInbox.pendingLightUpdates.front());
                state->networkInbox.pendingLightUpdates.pop_front();
                for (const SectionKey& key : state->world->ApplyLightUpdate(update))
                {
                    state->networkInbox.dirtySections.push_back(key);
                }
            }

            // Dirty sections (a local block edit, or a Block Update just
            // applied above) take priority over ordinary chunk-load
            // traffic, so an edit near the player doesn't sit behind a
            // backlog of newly-streaming chunks — see NetworkInbox::
            // dirtySections' own comment.
            if (!state->networkInbox.dirtySections.empty())
            {
                dirtySection = state->networkInbox.dirtySections.front();
                state->networkInbox.dirtySections.pop_front();
            }
            else if (!state->networkInbox.chunks.empty())
            {
                chunk = std::move(state->networkInbox.chunks.front());
                state->networkInbox.chunks.pop();
            }
            queueDepth = state->networkInbox.chunks.size();
            if (state->networkInbox.spawnPosition.has_value())
            {
                spawnPosition = state->networkInbox.spawnPosition;
                state->networkInbox.spawnPosition.reset();
            }
        }

        if (spawnPosition.has_value())
        {
            if (!state->worldReady.load())
            {
                // Initial spawn. Safe against RenderThread's concurrent
                // Tick() calls only because worldReady is still false
                // here and these writes precede (in this thread's program
                // order) the eventual worldReady.store(true) below — see
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
            // any edit could target it) — nothing to insert, just re-mesh
            // and upsert the one section. If the chunk unloaded/was never
            // there (a stale request racing a disconnect), there's nothing
            // to do.
            const Chunk* dirtyChunk = state->world->GetChunk(dirtySection->chunkX, dirtySection->chunkZ);
            if (dirtyChunk != nullptr)
            {
                SectionMeshTiming timing = MeshAndUpsertSection(state, textureManager, *dirtyChunk, *dirtySection);
                Log::Info("[MESH] section (" + std::to_string(dirtySection->chunkX) + ","
                    + std::to_string(dirtySection->chunkZ) + "," + std::to_string(dirtySection->sectionY) + "): "
                    + std::to_string(timing.cubeMs) + "ms cube + " + std::to_string(timing.nonCubicMs) + "ms non-cube");
            }
        }
        else if (chunk)
        {
            int chunkX = chunk->getX();
            int chunkZ = chunk->getZ();

            // Meshed section-by-section before InsertChunk below places this
            // chunk itself into World, but that's fine — ChunkMesher only
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
                totalCubeMs += timing.cubeMs;
                totalNonCubicMs += timing.nonCubicMs;
                meshedSections++;
            }
            state->world->InsertChunk(chunkX, chunkZ, std::move(*chunk));

            // Diagnostic timing — see whether a slow-to-appear chunk is
            // actually expensive to mesh, or just backed up behind
            // queueDepth others in this single-threaded queue (see this
            // loop's own comment on why there's no parallelism here yet).
            Log::Info("[MESH] chunk (" + std::to_string(chunkX) + "," + std::to_string(chunkZ) + "): "
                + std::to_string(totalCubeMs) + "ms cube + " + std::to_string(totalNonCubicMs) + "ms non-cube, "
                + std::to_string(meshedSections) + "/" + std::to_string(SECTIONS_PER_CHUNK) + " sections, "
                + std::to_string(queueDepth) + " queued");

            // worldReady must be the last write of this transition — see
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
            // empties — a big backlog can otherwise hog a core long enough
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
