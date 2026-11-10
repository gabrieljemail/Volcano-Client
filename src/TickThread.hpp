#pragma once
#ifndef VOLCANO_TICK_THREAD_H
#define VOLCANO_TICK_THREAD_H

#include <stop_token>
#include <thread>
#include "GlobalState.hpp"

namespace Volcano {

// Builds and sends this session's per-tick movement packets on its own
// steady 1/20s wall-clock cadence, independent of NetworkThread's blocking
// socket reads. Read-only w.r.t. player state
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
