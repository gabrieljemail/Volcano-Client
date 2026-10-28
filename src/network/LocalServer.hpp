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

// Launches and owns a singleplayer server process
class LocalServer {
public:
    LocalServer() = default;
    ~LocalServer();
    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;

    // Launches "<javaPath>" -jar server.jar <javaArguments> --port <port>
    // --nogui with `directory` as the working directory (so eula.txt,
    // server.properties, and the world save all resolve inside it). Returns
    // false - and logs why
    bool Start(const std::string& directory, const std::string& javaPath,
        const std::string& javaArguments, uint16_t port);

    // Called from DrainOutputPipe's thread for every line the server prints
    // to stdout/stderr, after it's already been logged with a [SERVER]
    // prefix - set (before Start()) by whoever wants to show it somewhere
    // else too, e.g. NetworkThread mirroring it into GlobalState's
    // serverLogLines for the starting-server screen to display live.
    std::function<void(const std::string&)> onOutputLine;

    // Waits for the server's own "Done (...)! For help, type "help"" log
    // line (see serverReadyForLogin)
    bool WaitUntilReady(std::stop_token stopToken);

    bool IsRunning() const;

    // Types a console command into the server's stdin, with full console
    // permissions. No-op if the server isn't running.
    void SendCommand(const std::string& command);

    // Asks the server to shut down cleanly
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
    // a clean Stop() call
    HANDLE jobHandle = nullptr;

    // Drains stdoutRead (the server's stdout AND stderr - both point at the
    // same pipe, see Start()) line by line, logging each with a [SERVER]
    // prefix, until the pipe breaks (the process exited and closed its
    // copies of the write end). Runs on its own thread since it blocks on
    // ReadFile for as long as the server is alive
    void DrainOutputPipe();
    std::thread outputReaderThread;
#endif
};

} // namespace Volcano

#endif
