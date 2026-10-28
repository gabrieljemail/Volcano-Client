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
// rather than introducing a separate abstraction. Runs on its own thread —
// deliberately NOT sharing one with glfwPollEvents() — so a burst of packet
// handling (e.g. a wave of chunk data right after login) can't stall input
// dispatch. See the networking plan for the full rationale.
class NetworkThread {
public:
    // localServerDir: non-empty for a singleplayer session (one of
    // settings.json's Network.LocalServerDirs entries) — ThreadEntry then
    // launches that server.jar and waits for host:port to come up before
    // connecting to it exactly like any other server. Empty for a normal
    // multiplayer connection, the default.
    NetworkThread(GlobalState* globalState, std::string host, uint16_t port, std::string username,
        std::string localServerDir = "");
    ~NetworkThread();
    void Start();

    // Signals ThreadEntry to stop WITHOUT waiting for it — unlike
    // destroying/replacing this object (which joins, and so blocks the
    // caller for as long as shutdown takes, up to LocalServer::Stop()'s own
    // 30s grace period), this is safe to call from the render thread even
    // mid-startup. Used by the starting-server screen's Cancel button: it
    // just needs the attempt abandoned, not confirmation it's fully torn
    // down before the click handler returns.
    void RequestStop();

    // True once the connection is closed and (for singleplayer) the local
    // server process has fully stopped — i.e. after RequestStop(), when it's
    // safe to reset world state without packets still arriving. The thread
    // itself stays alive/joinable after this, see ThreadEntry.
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
    // meant reconnecting — destroying the previous NetworkThread instance
    // via the connect button's unique_ptr reassignment — destroyed
    // ioContext out from under a worker thread that could still be using
    // it, which is undefined behavior for asio and hung the app
    // indefinitely instead of crashing outright.
    std::jthread worker;

    void ThreadEntry(std::stop_token stopToken);
};

} // namespace Volcano

#endif
