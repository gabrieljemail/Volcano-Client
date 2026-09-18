#include "NetworkThread.hpp"
#include "../Logger.hpp"
#include <chrono>
#include <memory>

namespace Volcano {

NetworkThread::NetworkThread(GlobalState* globalState, std::string host, uint16_t port, std::string username)
    : state(globalState), host(std::move(host)), port(port), username(std::move(username))
{}

NetworkThread::~NetworkThread() = default;

void NetworkThread::Start()
{
    worker = std::jthread([this](std::stop_token st) { ThreadEntry(st); });
}

void NetworkThread::ThreadEntry(std::stop_token stopToken)
{
    Log::Info("[INFO] Network thread created.");

    // Heap-allocated (rather than a stack local of this function) purely so
    // its Connection can be published to other threads safely — see the
    // aliasing shared_ptr below.
    auto client = std::make_shared<NetworkClient>(ioContext);
    // Drives TickLoop::Tick() from RunPlayLoop's own loop (see the
    // tick-loop plan) without NetworkClient needing to know TickLoop
    // exists at all — see SetTickCallback's own comment. The returned
    // IsGrounded() reflects the state Tick() (called first, this same
    // line) just computed.
    client->SetTickCallback([this] { state->tickLoop->Tick(); return state->tickLoop->IsGrounded(); });
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

    // The session/login attempt is over one way or another — tell the main
    // thread why, so it can reopen the connect screen, unless this was a
    // deliberate app-level stop (closing the game), which unblocks
    // RunSession's read loop the exact same way a kick does and shouldn't
    // pop the connect screen back up while the app is quitting.
    if (!stopToken.stop_requested() && !state->shouldClose) {
        std::string reason = client->GetLastError();
        state->ReportDisconnect(reason.empty() ? "Disconnected from server" : reason);
    }

    // Stay alive/joinable alongside the render thread even after the
    // session ends (login failure, disconnect, or stop requested).
    while (!stopToken.stop_requested() && !state->shouldClose)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace Volcano
