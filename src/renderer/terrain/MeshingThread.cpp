#include "MeshingThread.hpp"
#include "ChunkMesher.hpp"
#include "../misc/NonCubicMesher.hpp"
#include "Logger.hpp"
#include "TickLoop.hpp"
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

namespace Volcano {

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
    Log::Info("[INFO] Meshing thread created.");

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
        std::optional<glm::vec3> spawnPosition;

        {
            std::lock_guard<std::mutex> lock(state->networkInbox.mutex);
            if (!state->networkInbox.chunks.empty())
            {
                chunk = std::move(state->networkInbox.chunks.front());
                state->networkInbox.chunks.pop();
            }
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
                // Advance() calls only because worldReady is still false
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
                // Every later teleport: the render thread owns TickLoop's
                // simulation state once worldReady is set, so hand it over
                // rather than writing it from here.
                state->tickLoop->QueueTeleport(*spawnPosition);
            }
        }

        if (chunk)
        {
            int chunkX = chunk->getX();
            int chunkZ = chunk->getZ();

            // Meshed before InsertChunk below places this chunk itself into
            // World, but that's fine — ChunkMesher only needs World for
            // this chunk's neighbors (see its own comment), not itself.
            Mesh mesh = ChunkMesher::MeshChunk(*chunk, *state->world, *textureManager);
            Mesh nonCubicMesh = NonCubicMesher::MeshChunk(*chunk, *textureManager);
            state->world->InsertChunk(chunkX, chunkZ, std::move(*chunk));

            {
                // RenderThread iterates renderList on its own thread every
                // frame — see renderListMutex's comment in GlobalState.hpp.
                std::lock_guard<std::mutex> lock(state->renderListMutex);
                state->renderList.push_back(mesh);
            }
            {
                std::lock_guard<std::mutex> lock(state->nonCubicRenderListMutex);
                state->nonCubicRenderList.push_back(nonCubicMesh);
            }

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
        else if (!spawnPosition.has_value())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

} // namespace Volcano
