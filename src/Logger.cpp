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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include "Config.hpp"
#include "renderer/gui/models/Chat.hpp"

namespace Volcano::Log {

namespace {

namespace fs = std::filesystem;

enum class Level { Info, Warn, Error, Debug };

struct Entry
{
    Level level;
    std::string time; // HH:MM:SS.mmm
    int thread;
    std::string message;
};

constexpr const char* LOG_DIRECTORY = "logs";
constexpr const char* LATEST = "logs/latest.log";
constexpr size_t KEPT_OLD_LOGS = 10;
constexpr auto WRITE_INTERVAL = std::chrono::milliseconds(50);

// Plain statics, created on first use and never destroyed (a late log call during static destruction must not crash).
struct Sink
{
    std::mutex queueMutex;
    std::vector<Entry> queue;
    std::condition_variable wake;

    std::mutex writeMutex; // Serializes draining; held while touching the file and stdout.
    std::ofstream file;
    std::atomic<bool> standardOut{false};
    std::atomic<bool> stopping{false};
    std::thread writer;
};

int ThreadNumber()
{
    static std::atomic<int> next{1};
    thread_local int number = next.fetch_add(1);
    return number;
}

std::string Timestamp()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t seconds = system_clock::to_time_t(now);
    int millis = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec, millis);
    return buffer;
}

// Moves the previous session's latest.log aside and trims the oldest archives.
void RotateLogs()
{
    std::error_code ec;
    fs::create_directories(LOG_DIRECTORY, ec);
    if (fs::exists(LATEST, ec) && fs::file_size(LATEST, ec) > 0)
    {
        std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char name[64];
        std::strftime(name, sizeof(name), "%Y-%m-%d_%H.%M.%S.log", &local);
        fs::rename(LATEST, fs::path(LOG_DIRECTORY) / name, ec);
    }

    std::vector<fs::path> archives;
    for (const fs::directory_entry& entry : fs::directory_iterator(LOG_DIRECTORY, ec))
    {
        if (entry.path().extension() == ".log" && entry.path().filename() != "latest.log") archives.push_back(entry.path());
    }
    std::sort(archives.begin(), archives.end()); // Date-named, so oldest first.
    for (size_t i = 0; archives.size() > KEPT_OLD_LOGS && i < archives.size() - KEPT_OLD_LOGS; i++)
    {
        fs::remove(archives[i], ec);
    }
}

// Writes a batch to the file and, if enabled, stdout. Caller holds writeMutex.
void Write(Sink& sink, const std::vector<Entry>& batch)
{
    static const char* NAMES[] = { "INFO", "WARN", "ERROR", "DEBUG" };
    static const char* COLORS[] = { "\x1b[90m", "\x1b[33m", "\x1b[31m", "\x1b[90m" };
    bool out = sink.standardOut.load();
    for (const Entry& entry : batch)
    {
        int index = static_cast<int>(entry.level);
        char head[64];
        std::snprintf(head, sizeof(head), "[%s] [%s] [thread %d] ", entry.time.c_str(), NAMES[index], entry.thread);
        if (sink.file.is_open()) sink.file << head << entry.message << '\n';
        if (out) std::fprintf(stdout, "%s%s\x1b[0m%s\n", COLORS[index], head, entry.message.c_str());
    }
    if (sink.file.is_open()) sink.file.flush();
    if (out) std::fflush(stdout);
}

void Drain(Sink& sink)
{
    std::lock_guard<std::mutex> writing(sink.writeMutex);
    std::vector<Entry> batch;
    {
        std::lock_guard<std::mutex> queued(sink.queueMutex);
        batch.swap(sink.queue);
    }
    if (!batch.empty()) Write(sink, batch);
}

Sink& GetSink()
{
    static Sink* sink = []
    {
        Sink* created = new Sink();
        RotateLogs();
        created->file.open(LATEST, std::ios::out | std::ios::trunc);
        created->writer = std::thread([created]
        {
            while (!created->stopping.load())
            {
                {
                    std::unique_lock<std::mutex> lock(created->queueMutex);
                    created->wake.wait_for(lock, WRITE_INTERVAL, [&] { return created->stopping.load(); });
                }
                Drain(*created);
            }
        });
        return created;
    }();
    return *sink;
}

void Push(Level level, const std::string& message)
{
    Sink& sink = GetSink();
    Entry entry{ level, Timestamp(), ThreadNumber(), message };
    std::lock_guard<std::mutex> lock(sink.queueMutex);
    sink.queue.push_back(std::move(entry));
}

} // namespace

void Info(const std::string& message) { Push(Level::Info, message); }

void Warn(const std::string& message) { Push(Level::Warn, message); }

void Error(const std::string& message) { Push(Level::Error, message); }

void Debug(const std::string& message)
{
    Push(Level::Debug, message);
    // Off unless Debug.LogMessages asks for it (and before Config is loaded): diagnostics belong in the log.
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

void EnableStandardOut() { GetSink().standardOut = true; }

void Flush() { Drain(GetSink()); }

void Shutdown()
{
    Sink& sink = GetSink();
    if (sink.stopping.exchange(true)) return;
    sink.wake.notify_all();
    if (sink.writer.joinable()) sink.writer.join();
    Drain(sink);
}

Stream::~Stream()
{
    std::string text = buffer.str();
    switch (level)
    {
        case Level::Info: Info(text); break;
        case Level::Warn: Warn(text); break;
        case Level::Error: Error(text); break;
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

} // namespace Volcano::Log
