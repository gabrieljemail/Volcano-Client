#pragma once
#ifndef VOLCANO_CHAT_EVENT_H
#define VOLCANO_CHAT_EVENT_H

#include <vector>
#include "../TextComponent.hpp"

namespace Volcano {

// Decoded, display-ready form of an incoming System Chat Message or Player
// Chat Message packet — NetworkClient builds one of these from the raw NBT
// text component(s) in the packet (via TextComponent.hpp) and hands it to
// GUI::Chat, so packet parsing stays separate from how a message is shown.
struct ChatEvent {
    std::vector<ChatSegment> sender;  // empty for system messages (no sender to show)
    std::vector<ChatSegment> message;
};

} // namespace Volcano

#endif
