#pragma once
#ifndef VOLCANO_LOGGER_H
#define VOLCANO_LOGGER_H

#include <sstream>
#include <string>

namespace Volcano {

namespace Log {

// Logging is asynchronous: every call just queues a timestamped line, and one background thread writes the queue to
// logs/latest.log (and to stdout when enabled) about every 50 ms, so no game thread ever waits on I/O. Safe to call from
// any thread, before or after Init().

// Replaces plain std::cout usage
void Info(const std::string& message);

// Wide-string variant for Windows APIs that hand back WCHAR buffers
// (e.g. DXGI_ADAPTER_DESC::Description). Converts to UTF-8 and forwards to
// Info(const std::string&); const wchar_t*/L"..." convert implicitly.
void Info(const std::wstring& message);

// Replaces plain std::cerr usage for non-fatal problems
void Warn(const std::string& message);

// Replaces plain std::cerr usage
void Error(const std::string& message);

// Mirrored into the in-game chat window (GUI::Chat) when the Debug.LogMessages setting
// (Config::Active()) is true, so diagnostic output is visible without alt-tabbing to a terminal.
void Debug(const std::string& message);

// Debug.LogMeshEvents: whether the per-chunk/section "[MESH]" timing lines
// and the periodic "[HIZ]" culling stats get logged. Off by default (they
// fire constantly while moving); check it before building the message.
bool MeshEventsEnabled();

// Mirrors the log to stdout (colored) as well as the file. Off by default: the client is a windowed program and only
// writes logs/latest.log unless started with --stdout.
void EnableStandardOut();

// Writes everything queued so far, on the calling thread, before returning. The crash reporter calls this so the
// lines leading up to a crash reach the file.
void Flush();

// Flushes and stops the background writer. Idempotent; call before a normal exit.
void Shutdown();

// Stream-style helpers for code that used to write to std::cout/std::cerr:
//   Log::InfoStream() << "Found GPU: " << name;   // logged when the temporary ends
class Stream
{
public:
    enum class Level { Info, Warn, Error };
    explicit Stream(Level level) : level(level) {}
    ~Stream();
    template <typename T>
    Stream& operator<<(const T& value) { buffer << value; return *this; }
private:
    Level level;
    std::ostringstream buffer;
};
inline Stream InfoStream() { return Stream(Stream::Level::Info); }
inline Stream WarnStream() { return Stream(Stream::Level::Warn); }
inline Stream ErrorStream() { return Stream(Stream::Level::Error); }

} // namespace Log

} // namespace Volcano

#endif
