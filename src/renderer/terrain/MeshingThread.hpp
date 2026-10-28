#pragma once
#ifndef VOLCANO_MESHING_THREAD_H
#define VOLCANO_MESHING_THREAD_H

#include <atomic>
#include <thread>
#include "../../GlobalState.hpp"
#include "../TextureManager.hpp"

namespace Volcano {

// Hands a render-list mesh's slab regions back to whichever mesher owns
// them. Call with the owning render-list mutex held, as the mesh leaves it.
void ReleaseMeshSlabs(const Mesh& mesh);

// Consumes GlobalState::NetworkInbox (chunks + the one-time spawn position)
// on its own thread, decoupled from the network thread (which only parses
// bytes into plain Chunks) and from the main/render threads. Greedy meshing
// a full 16x384x16 chunk is real CPU work; running it inline on the main
// thread - as it used to, in VolcanoClient.cpp's DrainNetworkInbox
class MeshingThread {
public:
    MeshingThread(GlobalState* state, const TextureManager* textureManager);
    ~MeshingThread();

    void Start();
    void RequestStop();
    void Stop();

    // Blocks until the worker is idle between jobs (no section mid-mesh) and
    // keeps it there until Resume(). Render thread only; calls don't nest.
    // Returns immediately if the thread isn't running.
    void Pause();
    void Resume();

private:
    std::atomic<bool> pauseRequested{false};
    std::atomic<bool> paused{false};

    GlobalState* state;
    const TextureManager* textureManager;
    std::jthread worker;

    void ThreadEntry(std::stop_token stopToken);
};

} // namespace Volcano

#endif
