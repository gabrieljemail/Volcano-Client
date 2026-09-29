#include "NetworkThread.hpp"
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
    worker = std::jthread([this](std::stop_token st) { ThreadEntry(st); });
}

void NetworkThread::RequestStop()
{
    worker.request_stop();
}

void NetworkThread::ThreadEntry(std::stop_token stopToken)
{
    Log::Info("Network thread created.");

    // Singleplayer: launch this session's server.jar and wait for host:port
    // to actually accept connections before doing anything else — vanilla's
    // own singleplayer world load can take a while, and ConnectAndLogin
    // below has no retry loop of its own. skipConnect covers every way this
    // can fail (process never started, or exited before becoming ready) so
    // the fallthrough below doesn't also report its own, more generic
    // disconnect reason over a more specific one already given here.
    bool skipConnect = false;
    if (!localServerDir.empty())
    {
        std::string javaPath = std::get<std::string>(state->config->Get("Network.JavaPath", std::string("java")));
        std::string javaArguments = std::get<std::string>(state->config->Get("Network.JavaArguments", std::string("")));

        // Mirrors every logged server line into GlobalState::serverLogLines
        // too, for the starting-server screen to show live — see its own
        // comment. Cleared here (not by the GUI) so a new attempt doesn't
        // start with the previous one's tail still showing.
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
            state->ReportDisconnect("Failed to launch the local server — check Network.JavaPath in settings.json");
            skipConnect = true;
        }
        else if (!localServer.WaitUntilReady(stopToken))
        {
            localServer.Stop();
            if (!stopToken.stop_requested())
            {
                state->ReportDisconnect("The local server exited before finishing startup — see the server log");
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
    // not still go on to connect — the loading screen it came from is gone.
    if (!skipConnect && !stopToken.stop_requested())
    {
        // Heap-allocated (rather than a stack local of this function) purely
        // so its Connection can be published to other threads safely — see
        // the aliasing shared_ptr below.
        auto client = std::make_shared<NetworkClient>(ioContext);
        state->SetConnectionPhase(ConnectionPhase::Connecting, host + ":" + std::to_string(port));
        if (client->ConnectAndLogin(host, port, username)) {
            // Published only once Configuration is behind us and the Play loop
            // is about to start — see GlobalState::activeConnection's own
            // comment. Cleared unconditionally afterward: RunSession's internal
            // try/catch means it always returns normally, never throws, but
            // this doesn't rely on that — it runs whether or not it did.
            //
            // The aliasing constructor is what makes this safe to hand to other
            // threads: the published shared_ptr points at the Connection but
            // shares ownership of the whole NetworkClient, so a thread that
            // loaded a copy keeps the client (and therefore the socket) alive
            // for as long as it's still using it. The store(nullptr) below
            // releases this thread's reference; the object goes away on
            // whichever reference happens to be the last, instead of being
            // destroyed out from under a concurrent sender during shutdown.
            state->activeConnection.store(
                std::shared_ptr<Connection>(client, &client->GetConnection()));
            client->RunSession(state, stopToken);
            state->activeConnection.store(nullptr);
        }

        // Stop the local server (if any) before reporting the disconnect,
        // not after — ReportDisconnect is what pops the connect screen back
        // up, and the player shouldn't be able to hit "Connect" again (and
        // race a second java process against this one still shutting down)
        // before this one's fully gone.
        if (!localServerDir.empty()) localServer.Stop();

        // The session/login attempt is over one way or another — tell the main
        // thread why, so it can reopen the connect screen, unless this was a
        // deliberate app-level stop (closing the game), which unblocks
        // RunSession's read loop the exact same way a kick does and shouldn't
        // pop the connect screen back up while the app is quitting.
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
