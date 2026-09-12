#include "NetworkThread.hpp"
#include "../Logger.hpp"
#include <chrono>

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

    NetworkClient client(ioContext);
    if (client.ConnectAndLogin(host, port, username)) {
        // Published only once Configuration is behind us and the Play loop
        // is about to start — see GlobalState::activeConnection's own
        // comment. Cleared unconditionally afterward: RunSession's internal
        // try/catch means it always returns normally, never throws, but
        // this doesn't rely on that — it runs whether or not it did.
        state->activeConnection.store(&client.GetConnection());
        client.RunSession(state, stopToken);
        state->activeConnection.store(nullptr);
    }

    // Stay alive/joinable alongside the render thread even after the
    // session ends (login failure, disconnect, or stop requested).
    while (!stopToken.stop_requested() && !state->shouldClose)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace Volcano
