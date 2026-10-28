#include "Chat.hpp"
#include <utility>

namespace Volcano::GUI {

std::mutex Chat::mutex;
std::deque<ChatLine> Chat::lines;
std::atomic<uint64_t> Chat::revision{ 0 };

void Chat::Push(ChatLine line)
{
    std::lock_guard<std::mutex> lock(mutex);
    lines.push_back(std::move(line));
    while (lines.size() > MAX_LINES) lines.pop_front();
    revision++;
}

void Chat::AddMessage(const ChatEvent& event)
{
    ChatLine line;

    if (!event.sender.empty())
    {
        line.segments.push_back(ChatSegment{ "<", 0xAAAAAAu });
        for (const ChatSegment& segment : event.sender) line.segments.push_back(segment);
        line.segments.push_back(ChatSegment{ "> ", 0xAAAAAAu });
    }

    for (const ChatSegment& segment : event.message) line.segments.push_back(segment);

    Push(std::move(line));
}

void Chat::AddLine(const std::string& text, uint32_t colorRGB)
{
    ChatLine line;
    line.segments.push_back(ChatSegment{ text, colorRGB });
    Push(std::move(line));
}

std::vector<ChatLine> Chat::GetLines()
{
    std::lock_guard<std::mutex> lock(mutex);
    return std::vector<ChatLine>(lines.begin(), lines.end());
}

} // namespace Volcano::GUI
