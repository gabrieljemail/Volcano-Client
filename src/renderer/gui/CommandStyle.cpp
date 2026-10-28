#include "CommandStyle.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include "Theme.hpp"

namespace Volcano::GUI {

namespace {

using Pincel::Color;
namespace Syntax = Theme::Syntax;

// Paints byte ranges of the text, as the character positions Pincel wants. A later paint covers an earlier one.
struct Painter {
    const std::string& text;
    std::vector<size_t> charAt;                     // Byte offset -> character position.
    std::vector<Pincel::InputHighlight> runs;

    explicit Painter(const std::string& source) : text(source)
    {
        charAt.reserve(text.size() + 1);
        size_t chars = 0;
        for (unsigned char byte : text)
        {
            charAt.push_back(chars);
            if ((byte & 0xC0) != 0x80) chars++;     // Not a continuation byte: a character starts here.
        }
        charAt.push_back(chars);
    }

    void Paint(size_t start, size_t end, Color color)
    {
        end = std::min(end, text.size());
        if (start < end) runs.push_back({ charAt[start], charAt[end], color });
    }
};

bool IsWordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || std::strchr("_.+-/#", c) != nullptr;
}

bool IsNumber(const std::string& word)
{
    size_t i = !word.empty() && word[0] == '-' ? 1 : 0;
    size_t digits = 0;
    bool dot = false;
    for (; i < word.size(); i++)
    {
        if (std::isdigit(static_cast<unsigned char>(word[i]))) digits++;
        else if (word[i] == '.' && !dot) dot = true;
        else break;
    }
    return digits > 0 && (i == word.size() || (i + 1 == word.size() && std::strchr("bBsSlLfFdD", word[i]) != nullptr));
}

// minecraft:diamond_ore: the namespace in one color and the name in another. No namespace is all name.
void PaintId(Painter& painter, size_t start, size_t end)
{
    size_t colon = painter.text.find(':', start);
    if (colon == std::string::npos || colon >= end)
    {
        painter.Paint(start, end, Syntax::CLASS_NAME);
        return;
    }
    painter.Paint(start, colon, Syntax::NAMESPACE_NAME);
    painter.Paint(colon, end, Syntax::CLASS_NAME);
}

// Selectors, NBT, block states and the like: @e[type=ender_pearl,limit=1], {Item:{id:"minecraft:stone",Count:1b}}.
// Words that go before = or : are keys, and everything else is told apart by how it looks.
void PaintStructured(Painter& painter, size_t start, size_t end)
{
    const std::string& text = painter.text;
    int depth = 0;
    size_t i = start;
    while (i < end)
    {
        char c = text[i];
        if (c == ' ')
        {
            i++;
        }
        else if (c == '@' && i == start && i + 1 < end && std::isalpha(static_cast<unsigned char>(text[i + 1])))
        {
            painter.Paint(i, i + 2, Syntax::CLASS_NAME);
            i += 2;
        }
        else if (c == '"' || c == '\'')
        {
            size_t j = i + 1;
            while (j < end && text[j] != c) j += text[j] == '\\' ? 2 : 1;
            j = std::min(j + 1, end);
            painter.Paint(i, j, Syntax::STRING);
            i = j;
        }
        else if (std::strchr("[]{}(),=:;!<>", c) != nullptr)
        {
            if (c == '[' || c == '{') depth++;
            if ((c == ']' || c == '}') && depth > 0) depth--;
            painter.Paint(i, i + 1, Syntax::OPERATOR);
            i++;
        }
        else if (IsWordChar(c))
        {
            // A word, with a namespace colon kept inside it when a name follows the colon.
            size_t j = i;
            while (j < end && (IsWordChar(text[j]) || (text[j] == ':' && j + 1 < end && (std::isalpha(static_cast<unsigned char>(text[j + 1])) || text[j + 1] == '_' || text[j + 1] == '/')))) j++;
            std::string word = text.substr(i, j - i);
            size_t after = j;
            while (after < end && text[after] == ' ') after++;
            bool key = depth > 0 && after < end && (text[after] == '=' || text[after] == ':');

            if (key) painter.Paint(i, j, Syntax::KEY);
            else if (word == "true" || word == "false") painter.Paint(i, j, Syntax::CONST_PARAMETER);
            else if (IsNumber(word)) painter.Paint(i, j, Syntax::NUMBER);
            else PaintId(painter, i, j);
            i = j;
        }
        else
        {
            i++;
        }
    }
}

// An id at the start, then the [states] or {NBT} after it, for blocks and items.
void PaintIdWithData(Painter& painter, size_t start, size_t end)
{
    size_t data = painter.text.find_first_of("[{", start);
    if (data == std::string::npos || data >= end) data = end;
    PaintId(painter, start, data);
    PaintStructured(painter, data, end);
}

void PaintArgument(Painter& painter, const CommandToken& token)
{
    size_t start = token.start, end = token.end;
    switch (token.parser)
    {
    case CommandParser::Bool:
    case CommandParser::GameMode:
    case CommandParser::EntityAnchor:
    case CommandParser::Operation:
    case CommandParser::TemplateMirror:
    case CommandParser::TemplateRotation:
    case CommandParser::Heightmap:
    case CommandParser::Swizzle:
    case CommandParser::ScoreboardSlot:
    case CommandParser::Color: painter.Paint(start, end, Syntax::CONST_PARAMETER); break;

    case CommandParser::Integer:
    case CommandParser::Long:
    case CommandParser::Float:
    case CommandParser::Double:
    case CommandParser::Time:
    case CommandParser::Angle:
    case CommandParser::Rotation:
    case CommandParser::BlockPos:
    case CommandParser::ColumnPos:
    case CommandParser::Vec2:
    case CommandParser::Vec3:
    case CommandParser::IntRange:
    case CommandParser::FloatRange: painter.Paint(start, end, Syntax::NUMBER); break;

    case CommandParser::Entity:
    case CommandParser::GameProfile:
    case CommandParser::ScoreHolder:
    case CommandParser::Team:
        painter.Paint(start, end, Syntax::CLASS_NAME);
        PaintStructured(painter, start, end);
        break;

    case CommandParser::NbtPath: painter.Paint(start, end, Syntax::KEY); break;

    case CommandParser::Nbt:
    case CommandParser::NbtTag:
    case CommandParser::Component:
    case CommandParser::Style: PaintStructured(painter, start, end); break;

    case CommandParser::BlockState:
    case CommandParser::BlockPredicate:
    case CommandParser::ItemStack:
    case CommandParser::ItemPredicate: PaintIdWithData(painter, start, end); break;

    case CommandParser::ResourceLocation:
    case CommandParser::Function:
    case CommandParser::Dimension:
    case CommandParser::Resource:
    case CommandParser::ResourceKey:
    case CommandParser::ResourceOrTag:
    case CommandParser::ResourceOrTagKey:
    case CommandParser::ResourceSelector:
    case CommandParser::LootTable:
    case CommandParser::LootPredicate:
    case CommandParser::LootModifier:
    case CommandParser::Dialog:
    case CommandParser::Particle: PaintIdWithData(painter, start, end); break;

    default: painter.Paint(start, end, Syntax::STRING); break;
    }
}

} // namespace

std::vector<Pincel::InputHighlight> CommandStyle::Highlight(const std::string& text, const CommandAnalysis& analysis) const
{
    Painter painter(text);
    for (size_t i = 0; i < analysis.tokens.size(); i++)
    {
        const CommandToken& token = analysis.tokens[i];
        switch (token.kind)
        {
        case CommandToken::Kind::Literal: painter.Paint(token.start, token.end, i == 0 ? Syntax::COMMAND : Syntax::CONST_PARAMETER); break;
        case CommandToken::Kind::Partial: painter.Paint(token.start, token.end, Syntax::DIM); break;
        case CommandToken::Kind::Invalid: painter.Paint(token.start, token.end, Syntax::INVALID_WORD); break;
        case CommandToken::Kind::Argument: PaintArgument(painter, token); break;
        }
    }
    return std::move(painter.runs);
}

std::vector<Pincel::TextSpan> CommandStyle::UsageSpans(const CommandAnalysis& analysis) const
{
    std::vector<Pincel::TextSpan> spans;
    bool first = true;
    for (const CommandUsagePart& part : analysis.usage)
    {
        if (!spans.empty())
        {
            Pincel::TextSpan space;
            space.text = " ";
            spans.push_back(std::move(space));
        }
        Pincel::TextSpan span;
        span.text = part.text;
        if (part.choice) span.color = Syntax::DIM;
        else if (part.argument) span.color = Syntax::KEY;
        else span.color = first ? Syntax::COMMAND : Syntax::CONST_PARAMETER;
        if (part.current) span.weight = Pincel::FontWeight::Bold;
        spans.push_back(std::move(span));
        first = false;
    }
    return spans;
}

std::vector<std::string> CommandStyle::Words(const std::string& text, const CommandAnalysis& analysis) const
{
    std::vector<std::string> words;
    for (const CommandToken& token : analysis.tokens)
    {
        if (token.kind == CommandToken::Kind::Literal) words.push_back(text.substr(token.start, token.end - token.start));
    }
    return words;
}

} // namespace Volcano::GUI
