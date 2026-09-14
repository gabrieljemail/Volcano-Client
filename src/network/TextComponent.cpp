#include "TextComponent.hpp"
#include "Logger.hpp"
#include <nlohmann/json.hpp>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace Volcano {

namespace {

// Lazily loaded once (C++11 function-local statics are thread-safe to
// initialize) — the resource-pack's own flat key->format-string dictionary,
// used to turn a "translate" component's key (e.g. "gameMode.changed") into
// real text instead of just displaying the raw key.
const std::unordered_map<std::string, std::string>& Lang()
{
    static std::unordered_map<std::string, std::string> lang = [] {
        std::unordered_map<std::string, std::string> result;
        std::ifstream file("resources/minecraft/assets/minecraft/lang/en_us.json");
        if (!file) {
            Log::Error("[ERROR] TextComponent: couldn't open en_us.json — translation keys will show raw.");
            return result;
        }
        try {
            nlohmann::json j;
            file >> j;
            for (const auto& [key, value] : j.items())
                if (value.is_string()) result[key] = value.get<std::string>();
        } catch (const std::exception& e) {
            Log::Error(std::string("[ERROR] TextComponent: failed to parse en_us.json: ") + e.what());
        }
        return result;
    }();
    return lang;
}

// Expands a lang format string's Java-style %s / %1$s / %% placeholders,
// substituting each already-flattened "with" argument in order (plain %s)
// or by explicit 1-based index (%1$s, %2$s, ...) — Minecraft lang strings
// use both forms depending on the key.
void FormatTranslation(const std::string& format, const std::vector<std::vector<ChatSegment>>& args,
        uint32_t color, std::vector<ChatSegment>& out)
{
    std::string literal;
    size_t implicitIndex = 0;
    auto flushLiteral = [&]() {
        if (!literal.empty()) { out.push_back(ChatSegment{ literal, color }); literal.clear(); }
    };

    for (size_t i = 0; i < format.size(); i++) {
        if (format[i] != '%' || i + 1 >= format.size()) { literal += format[i]; continue; }

        size_t j = i + 1, numStart = j;
        while (j < format.size() && std::isdigit(static_cast<unsigned char>(format[j]))) j++;

        bool hasExplicitIndex = false;
        size_t explicitIndex = 0;
        if (j > numStart && j < format.size() && format[j] == '$') {
            hasExplicitIndex = true;
            explicitIndex = static_cast<size_t>(std::stoul(format.substr(numStart, j - numStart)));
            j++;
        }

        if (j < format.size() && format[j] == 's') {
            size_t argIndex = hasExplicitIndex ? explicitIndex - 1 : implicitIndex++;
            flushLiteral();
            if (argIndex < args.size())
                out.insert(out.end(), args[argIndex].begin(), args[argIndex].end());
            i = j;
        } else if (!hasExplicitIndex && j < format.size() && format[j] == '%') {
            literal += '%';
            i = j;
        } else {
            literal += format[i]; // Unrecognized specifier — show it literally rather than guessing.
        }
    }
    flushLiteral();
}

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
            {
                const std::string& key = std::get<std::string>(it->second.value);

                std::vector<std::vector<ChatSegment>> args;
                if (auto withIt = compound.find("with"); withIt != compound.end() && withIt->second.type == TagType::List)
                {
                    const TagList& withList = *std::get<std::unique_ptr<TagList>>(withIt->second.value);
                    for (const Tag& element : withList.elements)
                    {
                        std::vector<ChatSegment> argSegments;
                        Flatten(element, color, argSegments);
                        args.push_back(std::move(argSegments));
                    }
                }

                const auto& lang = Lang();
                if (auto langIt = lang.find(key); langIt != lang.end())
                    FormatTranslation(langIt->second, args, color, out);
                else
                    out.push_back(ChatSegment{ key, color }); // Unknown key — show it raw rather than nothing.
            }

            if (auto it = compound.find("extra"); it != compound.end())
                Flatten(it->second, color, out);
            return;
        }

        // Raw numbers legitimately appear as "with" array entries (teleport
        // coordinates, scores, ...) — sent as bare NBT numbers, not wrapped
        // in a {"text": ...} component. Dropping these (the previous
        // behavior) is what left commands.teleport.success's coordinates
        // blank ("Teleported VoidDev to , ,").
        case TagType::Byte:
            out.push_back(ChatSegment{ std::to_string(std::get<int8_t>(tag.value)), inheritedColor });
            return;
        case TagType::Short:
            out.push_back(ChatSegment{ std::to_string(std::get<int16_t>(tag.value)), inheritedColor });
            return;
        case TagType::Int:
            out.push_back(ChatSegment{ std::to_string(std::get<int32_t>(tag.value)), inheritedColor });
            return;
        case TagType::Long:
            out.push_back(ChatSegment{ std::to_string(std::get<int64_t>(tag.value)), inheritedColor });
            return;
        case TagType::Float: {
            std::ostringstream oss;
            oss.precision(2);
            oss << std::fixed << std::get<float>(tag.value);
            out.push_back(ChatSegment{ oss.str(), inheritedColor });
            return;
        }
        case TagType::Double: {
            std::ostringstream oss;
            oss.precision(2);
            oss << std::fixed << std::get<double>(tag.value);
            out.push_back(ChatSegment{ oss.str(), inheritedColor });
            return;
        }

        default:
            return; // Booleans/End/array types don't appear as real-world component nodes.
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
