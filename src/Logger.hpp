#pragma once
#ifndef VOLCANO_LOGGER_H
#define VOLCANO_LOGGER_H

#include <string>

namespace Volcano {

namespace Log {

// Replaces plain std::cout usage
void Info(const std::string& message);

// Wide-string variant for Windows APIs that hand back WCHAR buffers
// (e.g. DXGI_ADAPTER_DESC::Description). Converts to UTF-8 and forwards to
// Info(const std::string&); const wchar_t*/L"..." convert implicitly.
void Info(const std::wstring& message);

// Replaces plain std::cerr usage
void Error(const std::string& message);

// Always printed to the console like Info(); additionally mirrored into
// the in-game chat window (GUI::Chat) when the Debug.LogMessages setting
// (Config::Active()) is true, so diagnostic output is visible without
// alt-tabbing to a terminal.
void Debug(const std::string& message);

// Debug.LogMeshEvents: whether the per-chunk/section "[MESH]" timing lines
// and the periodic "[HIZ]" culling stats get logged. Off by default (they
// fire constantly while moving); check it before building the message.
bool MeshEventsEnabled();

} // namespace Log

} // namespace Volcano

#endif
