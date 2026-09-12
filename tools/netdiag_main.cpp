// Standalone diagnostic: runs the same NetworkClient::ConnectAndLogin +
// RunSession path the real client uses, without any Vulkan/GLFW/GUI
// dependency, so the Configuration/Play packet exchange can be iterated on
// quickly against the real dev server. Not part of the shipped client.
#include <asio.hpp>
#include <iostream>
#include <stop_token>
#include "../src/network/NetworkClient.hpp"

int main(int argc, char** argv)
{
    std::string host = argc > 1 ? argv[1] : "108.197.182.119";
    uint16_t port = static_cast<uint16_t>(argc > 2 ? std::stoi(argv[2]) : 25565);
    std::string username = argc > 3 ? argv[3] : "NetDiag";

    Volcano::GlobalState state{};

    asio::io_context ioContext;
    Volcano::NetworkClient client(ioContext);
    if (client.ConnectAndLogin(host, port, username)) {
        std::stop_source stopSource;
        client.RunSession(&state, stopSource.get_token());
    }

    std::cout << "[DIAG] Done." << std::endl;
    return 0;
}
