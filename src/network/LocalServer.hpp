#pragma once
#ifndef VOLCANO_LOCAL_SERVER_H
#define VOLCANO_LOCAL_SERVER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Volcano {

// Launches and owns a singleplayer server process - a `server.jar` run out
// of one of settings.json's Network.LocalServerDirs entries - so
// NetworkThread can connect to it over loopback exactly the way it connects
// to any other server. One instance per NetworkThread session; not
// copyable/movable since it owns a live OS process handle.
class LocalServer {
public:
    LocalServer() = default;
    ~LocalServer();
    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;

    // Launches "<javaPath>" -jar server.jar <javaArguments> --port <port>
    // --nogui with `directory` as the working directory (so eula.txt,
    // server.properties, and the world save all resolve inside it). Returns
    // false - and logs why - only if the process itself couldn't be
    // created; it does not wait for the server to actually finish starting,
    // see WaitUntilReady for that.
    bool Start(const std::string& directory, const std::string& javaPath,
        const std::string& javaArguments, uint16_t port);

    // Called from DrainOutputPipe's thread for every line the server prints
    // to stdout/stderr, after it's already been logged with a [SERVER]
    // prefix - set (before Start()) by whoever wants to show it somewhere
    // else too, e.g. NetworkThread mirroring it into GlobalState's
    // serverLogLines for the starting-server screen to display live.
    std::function<void(const std::string&)> onOutputLine;

    // Waits for the server's own "Done (...)! For help, type "help"" log
    // line (see serverReadyForLogin) - not a bare TCP connect, which
    // succeeds far earlier, right as the listener socket binds (vanilla
    // opens it well before the world/spawn is ready). Logging in during
    // that window doesn't reliably fail cleanly; it's the leading suspect
    // for a heap-corruption crash this session, on the client side, that
    // reproduced across several different faulting modules - the server
    // itself was never actually the one crashing. Also returns false if the
    // process exits on its own (a bad JavaPath, a missing eula.txt
    // acceptance, a port already in use, ...) or stopToken fires (the user
    // cancelled, or the app is closing). Deliberately has no timeout of its
    // own: a slow-starting server (a big modpack, a cold filesystem cache,
    // ...) shouldn't get auto-killed just for taking a while - only a
    // person choosing to cancel, or the process dying on its own, ends the
    // wait.
    bool WaitUntilReady(std::stop_token stopToken);

    bool IsRunning() const;

    // Types a console command into the server's stdin, with full console
    // permissions. No-op if the server isn't running.
    void SendCommand(const std::string& command);

    // Asks the server to shut down cleanly - the same "stop" console
    // command a real operator would type, sent over its redirected stdin -
    // and waits for it to exit, falling back to a hard kill if it doesn't in
    // time. Safe to call more than once, and safe to call when Start() was
    // never called or already failed.
    void Stop();

private:
    // Set once DrainOutputPipe (Windows only currently) sees the line
    // WaitUntilReady's comment describes. Plain cross-platform member (not
    // inside the #ifdef below) since WaitUntilReady itself must compile on
    // every platform.
    std::atomic<bool> serverReadyForLogin{false};

#ifdef _WIN32
    HANDLE processHandle = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;

    // Ties the server's lifetime to ours: if this process ever ends without
    // a clean Stop() call - a crash, a force-kill, anything - Windows tears
    // down every handle we held, including this job's, and this job's
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE is what actually kills the server
    // process at that point, instead of leaving it orphaned and still
    // consuming RAM with nothing connected to it. Assigned once in Start();
    // closing it in Stop() only ever runs after the process is already
    // confirmed dead, so it never triggers this on a process we're
    // deliberately still shutting down gracefully.
    HANDLE jobHandle = nullptr;

    // Drains stdoutRead (the server's stdout AND stderr - both point at the
    // same pipe, see Start()) line by line, logging each with a [SERVER]
    // prefix, until the pipe breaks (the process exited and closed its
    // copies of the write end). Runs on its own thread since it blocks on
    // ReadFile for as long as the server is alive - started by Start(),
    // joined by Stop().
    void DrainOutputPipe();
    std::thread outputReaderThread;
#endif
};

} // namespace Volcano

#endif
