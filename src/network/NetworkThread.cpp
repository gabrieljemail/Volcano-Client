#include "NetworkThread.hpp"
#include "../CrashReporter.hpp"
#include "../Logger.hpp"
#include <chrono>
#include <memory>

namespace Volcano {

NetworkThread::NetworkThread(GlobalState* globalState, std::string host, uint16_t port, std::string username,
    std::string localServerDir)
    : state(globalState), host(std::move(host)), port(port), username(std::move(username)),
      localServerDir(std::move(localServerDir))
{}

NetworkThread::~NetworkThread() = default;

void NetworkThread::Start()
{
    worker = std::jthread([this](std::stop_token st) { CrashReporter::InstallForThread(); ThreadEntry(st); });
}

void NetworkThread::RequestStop()
{
    worker.request_stop();
}

void NetworkThread::ThreadEntry(std::stop_token stopToken)
{
    Log::Info("Network thread created.");

    // Singleplayer: launch this session's server.jar and wait for host:port
    // to actually accept connections before doing anything else
    bool skipConnect = false;
    if (!localServerDir.empty())
    {
        std::string javaPath = std::get<std::string>(state->config->Get("Network.JavaPath", std::string("java")));
        std::string javaArguments = std::get<std::string>(state->config->Get("Network.JavaArguments", std::string("")));

        // Mirrors every logged server line into GlobalState::serverLogLines
        // too, for the starting-server screen to show live
        {
            std::lock_guard<std::mutex> lock(state->serverLogMutex);
            state->serverLogLines.clear();
        }
        localServer.onOutputLine = [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(state->serverLogMutex);
            state->serverLogLines.push_back(line);
            if (state->serverLogLines.size() > 500) state->serverLogLines.pop_front();
        };

        Log::Info("Starting local server in " + localServerDir);
        if (!localServer.Start(localServerDir, javaPath, javaArguments, port))
        {
            state->ReportDisconnect("Failed to launch the local server - check Network.JavaPath in settings.json");
            skipConnect = true;
        }
        else if (!localServer.WaitUntilReady(stopToken))
        {
            localServer.Stop();
            if (!stopToken.stop_requested())
            {
                state->ReportDisconnect("The local server exited before finishing startup - see the server log");
            }
            skipConnect = true;
        }
        else
        {
            // Singleplayer owner gets operator, same as vanilla's integrated
            // server. Persists in ops.json; re-running it is a harmless no-op.
            localServer.SendCommand("op " + username);
        }
    }

    // A cancel that landed just as the local server finished starting must
    // not still go on to connect
    if (!skipConnect && !stopToken.stop_requested())
    {
        // Heap-allocated (rather than a stack local of this function) purely
        // so its Connection can be published to other threads safely
        auto client = std::make_shared<NetworkClient>(ioContext);
        state->SetConnectionPhase(ConnectionPhase::Connecting, host + ":" + std::to_string(port));
        if (client->ConnectAndLogin(host, port, username)) {
            // Published only once Configuration is behind us and the Play loop
            // is about to start
            state->activeConnection.store(
                std::shared_ptr<Connection>(client, &client->GetConnection()));
            client->RunSession(state, stopToken);
            state->activeConnection.store(nullptr);
        }

        // Stop the local server (if any) before reporting the disconnect,
        // not after
        if (!localServerDir.empty()) localServer.Stop();

        // The session/login attempt is over one way or another
        if (!stopToken.stop_requested() && !state->shouldClose) {
            std::string reason = client->GetLastError();
            state->ReportDisconnect(reason.empty() ? "Disconnected from server" : reason);
        }
    }

    sessionEnded.store(true);

    // Stay alive/joinable alongside the render thread even after the
    // session ends (login failure, disconnect, or stop requested).
    while (!stopToken.stop_requested() && !state->shouldClose)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace Volcano
