#pragma once
#ifndef VOLCANO_NETWORK_THREAD_H
#define VOLCANO_NETWORK_THREAD_H

#include <asio.hpp>
#include <cstdint>
#include <stop_token>
#include <string>
#include <thread>
#include "../GlobalState.hpp"
#include "../TickLoop.hpp"
#include "NetworkClient.hpp"
#include "LocalServer.hpp"

namespace Volcano {

// Mirrors RenderThread's shape (owns its own jthread, started via Start())
// rather than introducing a separate abstraction. Runs on its own thread -
// deliberately NOT sharing one with glfwPollEvents() - so a burst of packet
// handling (e.g. a wave of chunk data right after login) can't stall input
// dispatch. See the networking plan for the full rationale.
class NetworkThread {
public:
    // localServerDir: non-empty for a singleplayer session (one of
    // settings.json's Network.LocalServerDirs entries) - ThreadEntry then
    // launches that server.jar and waits for host:port to come up before
    // connecting to it exactly like any other server. Empty for a normal
    // multiplayer connection, the default.
    NetworkThread(GlobalState* globalState, std::string host, uint16_t port, std::string username,
        std::string localServerDir = "");
    ~NetworkThread();
    void Start();

    // Signals ThreadEntry to stop WITHOUT waiting for it
    void RequestStop();

    // True once the connection is closed and (for singleplayer) the local
    // server process has fully stopped
    bool HasSessionEnded() const { return sessionEnded.load(); }

private:
    std::atomic<bool> sessionEnded{false};

    GlobalState* state;
    asio::io_context ioContext;
    std::string host;
    uint16_t port;
    std::string username;
    std::string localServerDir;
    LocalServer localServer;

    // Declared last so it destructs FIRST (members destruct in reverse
    // declaration order): jthread's destructor requests stop and joins,
    // which is what guarantees ThreadEntry has actually exited before
    // ioContext (which the thread uses the whole time it runs) gets torn
    // down below it. Declared before ioContext, as it originally was here,
    // meant reconnecting
    std::jthread worker;

    void ThreadEntry(std::stop_token stopToken);
};

} // namespace Volcano

#endif
