#include "NetworkThread.hpp"
#include <chrono>
#include <iostream>

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
    std::cout << "[INFO] Network thread created." << std::endl;

    NetworkClient client(ioContext);
    client.ConnectAndLogin(host, port, username);

    // Configuration/Play-state packet pumping (driven by ioContext.run())
    // lands in a later pass. For now, just stay alive/joinable alongside
    // the render thread instead of exiting the moment login finishes.
    while (!stopToken.stop_requested() && !state->shouldClose)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace Volcano
