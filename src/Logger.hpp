#pragma once
#ifndef VOLCANO_LOGGER_H
#define VOLCANO_LOGGER_H

#include <string>

namespace Volcano {

// Whether Log::Debug also mirrors its messages into the in-game chat
// window, not just the console. A plain constant for now, per the request
// that created it — swap this for a read from GlobalState::settings once a
// real settings UI exists, no call sites need to change.
constexpr bool DEBUG_MESSAGES = true;

namespace Log {

// Replaces plain std::cout usage — unconditional console output.
void Info(const std::string& message);

// Replaces plain std::cerr usage — unconditional console output.
void Error(const std::string& message);

// Always printed to the console like Info(); additionally mirrored into
// the in-game chat window (GUI::Chat) when DEBUG_MESSAGES is true, so
// diagnostic output is visible without alt-tabbing to a terminal.
void Debug(const std::string& message);

} // namespace Log

} // namespace Volcano

#endif
