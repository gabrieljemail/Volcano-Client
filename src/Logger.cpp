#include "Logger.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include "Config.hpp"
#include "renderer/gui/models/Chat.hpp"

namespace Volcano::Log {

void Info(const std::string& message)
{
    // '\n', not std::endl — a high-frequency caller (e.g. NetworkClient's
    // per-packet unhandled-packet log) forcing a real flush syscall on every
    // single line was blocking NetworkThread badly enough to visibly delay
    // chunk data packets behind it in a busy world. But never flushing until
    // the OS buffer fills has its own failure mode: a rare, isolated line
    // (a one-off error in an otherwise quiet stretch) can sit invisible
    // indefinitely instead of showing up. Bounded instead of all-or-nothing:
    // at most one real flush per FLUSH_INTERVAL_MS, whichever caller happens
    // to cross it, capping both the worst-case syscall rate and the
    // worst-case time before a line actually becomes visible.
    std::cout << "[INFO] " << message << '\n';

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
    std::cerr << "[ERROR] " << message << std::endl;
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
