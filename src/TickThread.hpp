#pragma once
#ifndef VOLCANO_TICK_THREAD_H
#define VOLCANO_TICK_THREAD_H

#include <stop_token>
#include <thread>
#include "GlobalState.hpp"

namespace Volcano {

// Builds and sends this session's per-tick movement packets on its own
// steady 1/20s wall-clock cadence, independent of NetworkThread's blocking
// socket reads. Read-only w.r.t. player state - only samples position and
// TickLoop's grounded/horizontalCollision (see TickLoop's class comment for
// why it no longer drives the physics itself).
//
// Idles (like MeshingThread) until GlobalState::worldReady is set and a
// connection exists, rather than being started/stopped per connection
// attempt - started once, immediately, alongside MeshingThread.
//
// Reaches the socket only through GlobalState::activeConnection's
// QueuePacket - never SendPacket directly, since this isn't the network
// thread (see Connection's own comment).
//
// Its per-tick send sequence mirrors the real client's tick (see
// LocalPlayer::sendPosition and Minecraft::tick in
// resources/minecraft-code): at most one movement packet, picking the
// narrowest variant that covers what actually changed, followed always by
// PlayC2S::ClientTickEnd to close the tick.
class TickThread {
public:
    explicit TickThread(GlobalState* state) : state(state) {}
    ~TickThread();

    void Start();

private:
    GlobalState* state;
    std::jthread worker;

    void ThreadEntry(std::stop_token stopToken);
};

} // namespace Volcano

#endif
