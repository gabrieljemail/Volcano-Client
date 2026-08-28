#include <iostream>
#include <thread>
#include <chrono>
#include <stop_token>
#include <GLFW/glfw3.h>
#include "renderer/RenderThread.hpp"
#include "renderer/VulkanInit.hpp"
#include "GlobalState.hpp"

using namespace std;

int main()
{
    // Let's keep this just for fun.
    cout << "[INFO] Hello, World!" << endl;

    // Initialize global state.
    Volcano::GlobalState state{};
    state.LoadSettings();

    // Create the render thread.
    Volcano::RenderThread renderer(&state);
    Init();
    renderer.Start();

    // Poll inputs and stay alive.
    while (!state.shouldClose)
    {
        glfwPollEvents();
        this_thread::sleep_for(chrono::milliseconds(5));
    }

    return 0;
}