// windows.h first, with the same discipline as engine/d3d11: NOMINMAX keeps
// its min/max macros off std::min/std::max, and parsing it before any project
// header keeps its `byte` typedef clear of Helpers.hpp's `using namespace
// std` + std::byte.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#ifdef _WIN32
#include <windows.h>
#endif

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

void Info(const std::wstring& message)
{
#ifdef _WIN32
    if (message.empty())
    {
        Info(std::string{});
        return;
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
    {
        Info("(wide string conversion failed)");
        return;
    }
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()),
        utf8.data(), needed, nullptr, nullptr);
    Info(utf8);
#else
    // wchar_t is UTF-32 on Linux/macOS - narrow lossily for the console.
    std::string narrow;
    narrow.reserve(message.size());
    for (wchar_t c : message)
    {
        narrow += (c >= 0 && c < 128) ? static_cast<char>(c) : '?';
    }
    Info(narrow);
#endif
}

void Error(const std::string& message)
{
    std::cerr << "\x1b[1;31m[ERROR]\x1b[31m " << message << "\x1b[0m" << std::endl;
}

void Debug(const std::string& message)
{
    std::cout << message << std::endl;
    // Off unless Debug.LogMessages asks for it (and before Config is loaded): diagnostics belong in the console.
    bool logMessages = Config::Active() != nullptr && std::get<bool>(Config::Active()->Get("Debug.LogMessages", false));
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
