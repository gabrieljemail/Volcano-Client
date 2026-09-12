#pragma once
#ifndef VOLCANO_MESHING_THREAD_H
#define VOLCANO_MESHING_THREAD_H

#include <thread>
#include "../../GlobalState.hpp"
#include "../TextureManager.hpp"

namespace Volcano {

// Consumes GlobalState::NetworkInbox (chunks + the one-time spawn position)
// on its own thread, decoupled from the network thread (which only parses
// bytes into plain Chunks) and from the main/render threads. Greedy meshing
// a full 16x384x16 chunk is real CPU work; running it inline on the main
// thread — as it used to, in VolcanoClient.cpp's DrainNetworkInbox — meant a
// burst of incoming chunks held networkInbox.mutex for a while, stalling the
// network thread's own packet loop (Keep Alive replies included) right when
// the most world data is streaming in.
//
// The meshing work itself doesn't need to run on any particular thread:
// SlabBuffer::Allocate is a bare memcpy into persistently-mapped
// host-visible memory (no command buffer, no queue submission involved),
// and World already has its own shared_mutex for exactly this kind of
// cross-thread access. renderList is guarded by GlobalState::renderListMutex
// the same way it is against RenderThread.
//
// Must be fully stopped (Stop(), which joins) before VulkanInit::Cleanup()
// runs — it calls ChunkMesher::Shutdown(), which destroys the slab buffers
// this thread writes into.
class MeshingThread {
public:
    MeshingThread(GlobalState* state, const TextureManager* textureManager);
    ~MeshingThread();

    void Start();
    void RequestStop();
    void Stop();

private:
    GlobalState* state;
    const TextureManager* textureManager;
    std::jthread worker;

    void ThreadEntry(std::stop_token stopToken);
};

} // namespace Volcano

#endif
