#include <iostream>
#include <thread>
#include <stop_token>
#include "renderer/RenderThread.hpp"

using namespace std;

int main()
{
    // Let's keep this just for fun.
    cout << "[INFO] Hello, World!" << endl;

    // Create the render thread.
    Volcano::RenderThread renderer;

    return 0;
}