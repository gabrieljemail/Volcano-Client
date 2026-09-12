#pragma once
#ifndef GUI_CHAT_H
#define GUI_CHAT_H

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include "../../../network/TextComponent.hpp"
#include "../../../network/models/ChatEvent.hpp"

namespace Volcano::GUI {

struct ChatLine {
    std::vector<ChatSegment> segments;
};

// Thread-safe, process-wide chat/log scrollback. A single store backs both
// the in-game chat window (GUIController::Render reads a snapshot every
// frame) and Log::Debug's optional chat mirroring — the network thread and
// the main/render thread both append to it, so every access goes through
// `mutex`.
//
// A static singleton, like GUIController itself, rather than something
// reached via GlobalState: Log:: call sites deep in networking code
// (Connection.cpp, MSAuth.cpp, ...) have no GlobalState pointer to thread
// through, and there's only ever one chat log for the whole client anyway.
class Chat {
public:
    static void AddMessage(const ChatEvent& event);
    static void AddLine(const std::string& text, uint32_t colorRGB = 0xFFFFFFu);

    // Snapshot copy, so callers (GUIController::Render) don't hold `mutex`
    // while calling into ImGui.
    static std::vector<ChatLine> GetLines();

    static constexpr size_t MAX_LINES = 200;

private:
    static std::mutex mutex;
    static std::deque<ChatLine> lines;

    static void Push(ChatLine line);
};

} // namespace Volcano::GUI

#endif
