#pragma once
#ifndef VOLCANO_LOGGER_H
#define VOLCANO_LOGGER_H

#include <string>

namespace Volcano {

namespace Log {

// Replaces plain std::cout usage — unconditional console output.
void Info(const std::string& message);

// Replaces plain std::cerr usage — unconditional console output.
void Error(const std::string& message);

// Always printed to the console like Info(); additionally mirrored into
// the in-game chat window (GUI::Chat) when the Debug.LogMessages setting
// (Config::Active()) is true, so diagnostic output is visible without
// alt-tabbing to a terminal.
void Debug(const std::string& message);

} // namespace Log

} // namespace Volcano

#endif
