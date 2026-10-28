#include "Logger.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include "Config.hpp"
#include "renderer/gui/models/Chat.hpp"

namespace Volcano::Log {

void Info(const std::string& message)
{
    // Do not use std::endl(), it flushes the buffer every time.
    std::cout << "\x1b[90m[INFO]\x1b[0m " << message << '\n';

    using namespace std::chrono;
    static std::atomic<int64_t> lastFlushMs{0};
    constexpr int64_t FLUSH_INTERVAL_MS = 100;

    int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    int64_t last = lastFlushMs.load(std::memory_order_relaxed);
    if (now - last >= FLUSH_INTERVAL_MS && lastFlushMs.compare_exchange_strong(last, now, std::memory_order_relaxed))
    {
        std::cout.flush();
    }
}

void Error(const std::string& message)
{
    std::cerr << "\x1b[1;31m[ERROR]\x1b[31m " << message << "\x1b[0m" << std::endl;
}

void Debug(const std::string& message)
{
    std::cout << message << std::endl;
    // Config isn't loaded yet for any Debug() call made before
    // GlobalState::LoadSettings runs — default to on, same as before Config
    // existed, rather than silently dropping those early messages.
    bool logMessages = Config::Active() == nullptr || std::get<bool>(Config::Active()->Get("Debug.LogMessages", true));
    if (logMessages)
    {
        GUI::Chat::AddLine(message, 0xAAAAAAu); // dim gray, distinct from normal chat/system white
    }
}

bool MeshEventsEnabled()
{
    Config* config = Config::Active();
    return config != nullptr && std::get<bool>(config->Get("Debug.LogMeshEvents", false));
}

} // namespace Volcano::Log
