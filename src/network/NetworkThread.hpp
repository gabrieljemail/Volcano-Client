#pragma once
#ifndef VOLCANO_NETWORK_THREAD_H
#define VOLCANO_NETWORK_THREAD_H

#include <asio.hpp>
#include <cstdint>
#include <stop_token>
#include <string>
#include <thread>
#include "../GlobalState.hpp"
#include "NetworkClient.hpp"

namespace Volcano {

// Mirrors RenderThread's shape (owns its own jthread, started via Start())
// rather than introducing a separate abstraction. Runs on its own thread —
// deliberately NOT sharing one with glfwPollEvents() — so a burst of packet
// handling (e.g. a wave of chunk data right after login) can't stall input
// dispatch. See the networking plan for the full rationale.
class NetworkThread {
public:
    NetworkThread(GlobalState* globalState, std::string host, uint16_t port, std::string username);
    ~NetworkThread();
    void Start();

private:
    GlobalState* state;
    std::jthread worker;
    asio::io_context ioContext;
    std::string host;
    uint16_t port;
    std::string username;

    void ThreadEntry(std::stop_token stopToken);
};

} // namespace Volcano

#endif
