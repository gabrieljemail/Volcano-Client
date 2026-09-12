#include "TextComponent.hpp"
#include <sstream>
#include <unordered_map>
#include <utility>

namespace Volcano {

namespace {

const std::unordered_map<std::string, uint32_t>& NamedColors()
{
    // The 16 legacy Minecraft chat colors, keyed by their component "color" name.
    static const std::unordered_map<std::string, uint32_t> colors = {
        {"black", 0x000000}, {"dark_blue", 0x0000AA}, {"dark_green", 0x00AA00},
        {"dark_aqua", 0x00AAAA}, {"dark_red", 0xAA0000}, {"dark_purple", 0xAA00AA},
        {"gold", 0xFFAA00}, {"gray", 0xAAAAAA}, {"dark_gray", 0x555555},
        {"blue", 0x5555FF}, {"green", 0x55FF55}, {"aqua", 0x55FFFF},
        {"red", 0xFF5555}, {"light_purple", 0xFF55FF}, {"yellow", 0xFFFF55},
        {"white", 0xFFFFFF},
    };
    return colors;
}

uint32_t ResolveColor(const std::string& value, uint32_t inherited)
{
    if (!value.empty() && value[0] == '#')
    {
        try { return static_cast<uint32_t>(std::stoul(value.substr(1), nullptr, 16)); }
        catch (const std::exception&) { return inherited; }
    }

    const auto& colors = NamedColors();
    auto it = colors.find(value);
    return it != colors.end() ? it->second : inherited;
}

void Flatten(const NBT::Tag& tag, uint32_t inheritedColor, std::vector<ChatSegment>& out)
{
    using namespace NBT;

    switch (tag.type)
    {
        case TagType::String:
            out.push_back(ChatSegment{ std::get<std::string>(tag.value), inheritedColor });
            return;

        case TagType::List: {
            const TagList& list = *std::get<std::unique_ptr<TagList>>(tag.value);
            for (const Tag& element : list.elements) Flatten(element, inheritedColor, out);
            return;
        }

        case TagType::Compound: {
            const TagCompound& compound = *std::get<std::unique_ptr<TagCompound>>(tag.value);

            uint32_t color = inheritedColor;
            if (auto it = compound.find("color"); it != compound.end() && it->second.type == TagType::String)
                color = ResolveColor(std::get<std::string>(it->second.value), inheritedColor);

            if (auto it = compound.find("text"); it != compound.end() && it->second.type == TagType::String)
                out.push_back(ChatSegment{ std::get<std::string>(it->second.value), color });
            else if (auto it = compound.find("translate"); it != compound.end() && it->second.type == TagType::String)
                out.push_back(ChatSegment{ std::get<std::string>(it->second.value), color });

            if (auto it = compound.find("extra"); it != compound.end())
                Flatten(it->second, color, out);
            return;
        }

        default:
            return; // Numbers/booleans/End don't appear as real-world component nodes.
    }
}

} // namespace

std::vector<ChatSegment> ParseTextComponent(const NBT::Tag& tag)
{
    std::vector<ChatSegment> segments;
    Flatten(tag, 0xFFFFFFu, segments);
    return segments;
}

std::vector<ChatSegment> ReadTextComponent(PacketReader& reader)
{
    auto type = static_cast<NBT::TagType>(reader.ReadByte());
    if (type == NBT::TagType::End) return {};

    // Network NBT (used for packet fields like this one) omits the tag
    // name that file NBT always carries — read the payload straight off a
    // view of the reader's remaining bytes via an istream (what NBT::
    // ReadPayload expects), then re-sync `reader` by however many bytes
    // that actually consumed.
    std::string buffer(reinterpret_cast<const char*>(reader.Cursor()), reader.Remaining());
    std::istringstream stream(buffer, std::ios::binary);
    NBT::TagValue value = NBT::ReadPayload(stream, type);
    reader.Skip(static_cast<size_t>(stream.tellg()));

    return ParseTextComponent(NBT::Tag{ type, std::move(value) });
}

std::string PlainText(const std::vector<ChatSegment>& segments)
{
    std::string result;
    for (const ChatSegment& segment : segments) result += segment.text;
    return result;
}

} // namespace Volcano
