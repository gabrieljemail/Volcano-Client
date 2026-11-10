#pragma once
#ifndef VOLCANO_CHAT_EVENT_H
#define VOLCANO_CHAT_EVENT_H

#include <vector>
#include "../TextComponent.hpp"

namespace Volcano {

// Decoded, display-ready form of an incoming System Chat Message or Player
// Chat Message packet
struct ChatEvent {
    std::vector<ChatSegment> sender;  // empty for system messages (no sender to show)
    std::vector<ChatSegment> message;
};

} // namespace Volcano

#endif
