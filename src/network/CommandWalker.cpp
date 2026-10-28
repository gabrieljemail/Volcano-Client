#include "CommandWalker.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace Volcano {

namespace {

constexpr size_t MAX_SUGGESTIONS = 64;
constexpr size_t MAX_USAGE_OPTIONS = 5;

const char* const GAME_MODES[] = { "survival", "creative", "adventure", "spectator" };
const char* const ANCHORS[] = { "feet", "eyes" };
const char* const DIMENSIONS[] = { "minecraft:overworld", "minecraft:the_nether", "minecraft:the_end" };
const char* const OPERATIONS[] = { "=", "+=", "-=", "*=", "/=", "%=", "<", ">", "><" };
const char* const SELECTORS[] = { "@a", "@e", "@n", "@p", "@r", "@s" };

template<size_t N>
bool OneOf(const std::string& word, const char* const (&values)[N])
{
    for (const char* value : values)
    {
        if (word == value) return true;
    }
    return false;
}

bool StartsWith(const std::string& text, const std::string& prefix)
{
    if (prefix.size() > text.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++)
    {
        if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    }
    return true;
}

template<size_t N>
bool PrefixOfOne(const std::string& word, const char* const (&values)[N])
{
    for (const char* value : values)
    {
        if (StartsWith(value, word)) return true;
    }
    return false;
}

// Digits with an optional leading minus, and one dot if decimals are allowed. Not hex, not exponents.
bool NumberShape(const std::string& word, bool decimal, size_t& digits)
{
    digits = 0;
    bool dot = false;
    for (size_t i = word.empty() || word[0] != '-' ? 0 : 1; i < word.size(); i++)
    {
        if (std::isdigit(static_cast<unsigned char>(word[i]))) digits++;
        else if (decimal && word[i] == '.' && !dot) dot = true;
        else return false;
    }
    return true;
}

bool ParseNumber(const std::string& word, bool decimal, double& value)
{
    size_t digits;
    if (!NumberShape(word, decimal, digits) || digits == 0) return false;
    value = std::strtod(word.c_str(), nullptr);
    return true;
}

bool InRange(const CommandNode& node, double value)
{
    return (!node.min || value >= *node.min) && (!node.max || value <= *node.max);
}

// "~", "^", either with an offset, or a plain number.
bool IsCoordinate(const std::string& word, bool partial)
{
    size_t digits;
    size_t from = !word.empty() && (word[0] == '~' || word[0] == '^') ? 1 : 0;
    if (from == 1 && word.size() == 1) return true;
    if (!NumberShape(word.substr(from), true, digits)) return false;
    return partial || digits > 0;
}

int WordsNeeded(CommandParser parser)
{
    switch (parser)
    {
    case CommandParser::BlockPos:
    case CommandParser::Vec3: return 3;
    case CommandParser::ColumnPos:
    case CommandParser::Vec2:
    case CommandParser::Rotation: return 2;
    default: return 1;
    }
}

bool WordFits(const CommandNode& node, const std::string& word)
{
    double value;
    switch (node.parser)
    {
    case CommandParser::Bool: return word == "true" || word == "false";
    case CommandParser::Integer:
    case CommandParser::Long: return ParseNumber(word, false, value) && InRange(node, value);
    case CommandParser::Float:
    case CommandParser::Double: return ParseNumber(word, true, value) && InRange(node, value);
    case CommandParser::GameMode: return OneOf(word, GAME_MODES);
    case CommandParser::EntityAnchor: return OneOf(word, ANCHORS);
    case CommandParser::BlockPos:
    case CommandParser::ColumnPos:
    case CommandParser::Vec2:
    case CommandParser::Vec3:
    case CommandParser::Rotation:
    case CommandParser::Angle: return IsCoordinate(word, false);
    default: return !word.empty();
    }
}

// Whether `word`, still being typed, could become one this argument accepts.
bool WordCouldFit(const CommandNode& node, const std::string& word)
{
    size_t digits;
    switch (node.parser)
    {
    case CommandParser::Bool: return StartsWith("true", word) || StartsWith("false", word);
    case CommandParser::Integer:
    case CommandParser::Long: return NumberShape(word, false, digits);
    case CommandParser::Float:
    case CommandParser::Double: return NumberShape(word, true, digits);
    case CommandParser::GameMode: return PrefixOfOne(word, GAME_MODES);
    case CommandParser::EntityAnchor: return PrefixOfOne(word, ANCHORS);
    case CommandParser::BlockPos:
    case CommandParser::ColumnPos:
    case CommandParser::Vec2:
    case CommandParser::Vec3:
    case CommandParser::Rotation:
    case CommandParser::Angle: return IsCoordinate(word, true);
    default: return true;
    }
}

// The end of the word at `start`. Spaces inside quotes, braces and brackets don't end it (NBT, components).
size_t ScanWord(const std::string& text, size_t start)
{
    int depth = 0;
    char quote = 0;
    size_t pos = start;
    for (; pos < text.size(); pos++)
    {
        char c = text[pos];
        if (quote != 0)
        {
            if (c == '\\' && pos + 1 < text.size()) pos++;
            else if (c == quote) quote = 0;
        }
        else if ((c == '"' || c == '\'') && (pos == start || depth > 0)) quote = c;
        else if (c == '{' || c == '[') depth++;
        else if ((c == '}' || c == ']') && depth > 0) depth--;
        else if (c == ' ' && depth == 0) break;
    }
    return pos;
}

std::vector<std::string> ValuesOf(const CommandNode& node, const std::vector<std::string>& players)
{
    std::vector<std::string> values;
    auto addSelectors = [&](bool entities)
    {
        for (const char* selector : SELECTORS)
        {
            if (entities || (selector[1] != 'e' && selector[1] != 'n')) values.push_back(selector);
        }
    };
    auto addAll = [&](const auto& list) { for (const char* value : list) values.push_back(value); };

    switch (node.parser)
    {
    case CommandParser::Bool: values = { "true", "false" }; break;
    case CommandParser::GameMode: addAll(GAME_MODES); break;
    case CommandParser::EntityAnchor: addAll(ANCHORS); break;
    case CommandParser::Dimension: addAll(DIMENSIONS); break;
    case CommandParser::Operation: addAll(OPERATIONS); break;
    case CommandParser::Entity:
        values = players;
        addSelectors((node.flags & 0x02) == 0);
        break;
    case CommandParser::GameProfile:
    case CommandParser::ScoreHolder:
        values = players;
        addSelectors(node.parser == CommandParser::ScoreHolder);
        break;
    case CommandParser::BlockPos:
    case CommandParser::ColumnPos:
    case CommandParser::Vec2:
    case CommandParser::Vec3:
    case CommandParser::Rotation: values = { "~" }; break;
    default: break;
    }
    return values;
}

constexpr int MAX_AHEAD = 8;

// "block_pos" -> "BlockPos"; the plain value types stay lowercase.
std::string PrettyParser(CommandParser parser)
{
    switch (parser)
    {
    case CommandParser::Bool: return "bool";
    case CommandParser::Integer: return "int";
    case CommandParser::Long: return "long";
    case CommandParser::Float: return "float";
    case CommandParser::Double: return "double";
    case CommandParser::String: return "string";
    default: break;
    }
    std::string pretty;
    bool upper = true;
    for (char c : std::string(CommandTree::ParserName(parser)))
    {
        if (c == '_')
        {
            upper = true;
            continue;
        }
        pretty += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        upper = false;
    }
    return pretty;
}

std::vector<CommandUsagePart> MatchedUsage(const CommandAnalysis& analysis)
{
    std::vector<CommandUsagePart> usage;
    for (const CommandToken& token : analysis.tokens)
    {
        if (token.label.empty()) continue;
        CommandUsagePart part;
        part.text = token.label;
        part.argument = token.kind == CommandToken::Kind::Argument;
        usage.push_back(std::move(part));
    }
    return usage;
}

} // namespace

// A multi-word argument (a position) takes all its words; a greedy string takes the rest.
CommandWalker::Match CommandWalker::TryArgument(const CommandNode& node, int32_t index, const std::string& input, size_t start) const
{
    Match match;
    if (node.parser == CommandParser::String && node.stringMode == 2)
    {
        match.node = index;
        match.end = input.size();
        return match;
    }

    size_t pos = start;
    int words = WordsNeeded(node.parser);
    for (int i = 0; i < words; i++)
    {
        while (i > 0 && pos < input.size() && input[pos] == ' ') pos++;
        if (pos >= input.size())
        {
            match.incomplete = true;
            match.end = input.size();
            return match;
        }
        size_t end = ScanWord(input, pos);
        if (!WordFits(node, input.substr(pos, end - pos))) return Match{};
        pos = end;
    }
    match.node = index;
    match.end = pos;
    return match;
}

// A literal that is the word, else the first argument that takes it.
CommandWalker::Match CommandWalker::Choose(const std::vector<int32_t>& options, const std::string& input, size_t start) const
{
    size_t plainEnd = input.find(' ', start);
    if (plainEnd == std::string::npos) plainEnd = input.size();
    std::string word = input.substr(start, plainEnd - start);

    for (int32_t index : options)
    {
        const CommandNode& node = *tree->Node(index);
        if (node.type == CommandNode::Type::Literal && node.name == word)
        {
            Match match;
            match.node = index;
            match.end = plainEnd;
            return match;
        }
    }
    for (int32_t index : options)
    {
        const CommandNode& node = *tree->Node(index);
        if (node.type != CommandNode::Type::Argument) continue;
        Match match = TryArgument(node, index, input, start);
        if (match.node >= 0 || match.incomplete) return match;
    }
    return Match{};
}

bool CommandWalker::CouldFit(const std::vector<int32_t>& options, const std::string& word) const
{
    for (int32_t index : options)
    {
        const CommandNode& node = *tree->Node(index);
        if (node.type == CommandNode::Type::Literal ? StartsWith(node.name, word) : WordCouldFit(node, word)) return true;
    }
    return false;
}

void CommandWalker::Suggest(const std::vector<int32_t>& options, const std::string& partial,
    const std::vector<std::string>& players, CommandAnalysis& analysis) const
{
    std::vector<CommandSuggestion> literals, values, hints;
    for (int32_t index : options)
    {
        const CommandNode& node = *tree->Node(index);
        bool more = !tree->Next(node).empty();
        if (node.type == CommandNode::Type::Literal)
        {
            if (StartsWith(node.name, partial)) literals.push_back({ node.name, node.name, false, more, true });
            continue;
        }
        for (const std::string& value : ValuesOf(node, players))
        {
            if (StartsWith(value, partial)) values.push_back({ value, value, false, more, false });
        }
        hints.push_back({ "", Label(node), true, false, false });
    }

    auto byText = [](const CommandSuggestion& a, const CommandSuggestion& b) { return a.text < b.text; };
    auto sameText = [](const CommandSuggestion& a, const CommandSuggestion& b) { return a.text == b.text; };
    for (std::vector<CommandSuggestion>* list : { &literals, &values })
    {
        std::sort(list->begin(), list->end(), byText);
        list->erase(std::unique(list->begin(), list->end(), sameText), list->end());
    }

    for (std::vector<CommandSuggestion>* list : { &literals, &values, &hints })
    {
        for (CommandSuggestion& suggestion : *list)
        {
            if (analysis.suggestions.size() < MAX_SUGGESTIONS) analysis.suggestions.push_back(std::move(suggestion));
        }
    }
}

std::string CommandWalker::Label(const CommandNode& node) const
{
    if (node.type == CommandNode::Type::Literal) return node.name;
    std::string label = "<" + PrettyParser(node.parser) + " " + node.name;
    if (node.parser == CommandParser::Entity && (node.flags & 0x01)) label += "[1]"; // Takes one target.
    return label + ">";
}

// Follows the way on from `options` while there is only one, up to a node the command can end at. Where there is a
// choice it adds one part listing them.
void CommandWalker::AppendAhead(std::vector<CommandUsagePart>& usage, std::vector<int32_t> options, bool firstIsCurrent) const
{
    for (int i = 0; i < MAX_AHEAD && !options.empty(); i++)
    {
        CommandUsagePart part;
        part.current = firstIsCurrent && i == 0;
        if (options.size() > 1)
        {
            part.choice = true;
            part.text = "(";
            size_t shown = 0;
            for (int32_t index : options)
            {
                if (shown == MAX_USAGE_OPTIONS)
                {
                    part.text += " | ...";
                    break;
                }
                part.text += (shown++ > 0 ? " | " : "") + Label(*tree->Node(index));
            }
            part.text += ")";
            usage.push_back(std::move(part));
            return;
        }
        const CommandNode& node = *tree->Node(options[0]);
        part.text = Label(node);
        part.argument = node.type == CommandNode::Type::Argument;
        usage.push_back(std::move(part));
        if (node.executable) return;
        options = tree->Next(node);
    }
}

CommandAnalysis CommandWalker::Analyze(const std::string& input, const std::vector<std::string>& players) const
{
    CommandAnalysis analysis;
    if (!tree) return analysis;

    std::vector<int32_t> options = tree->Next(tree->Root());
    const CommandNode* last = nullptr;
    size_t pos = 0;
    while (true)
    {
        while (pos < input.size() && input[pos] == ' ') pos++;
        analysis.partialStart = pos;
        if (pos >= input.size())
        {
            analysis.executable = last != nullptr && last->executable;
            Suggest(options, "", players, analysis);
            analysis.usage = MatchedUsage(analysis);
            AppendAhead(analysis.usage, options, true);
            return analysis;
        }

        Match match = Choose(options, input, pos);
        if (match.node < 0 && !match.incomplete)
        {
            // Nothing took it. Still typing it if it runs to the end, with no space after.
            size_t end = ScanWord(input, pos);
            bool typing = end >= input.size();
            std::string word = input.substr(pos, end - pos);
            CommandToken token;
            token.start = pos;
            token.end = input.size();
            if (typing && CouldFit(options, word))
            {
                token.kind = CommandToken::Kind::Partial;
                analysis.tokens.push_back(token);
                Suggest(options, word, players, analysis);
            }
            else
            {
                token.kind = CommandToken::Kind::Invalid;
                analysis.tokens.push_back(token);
                analysis.failed = true;
            }
            analysis.usage = MatchedUsage(analysis);
            AppendAhead(analysis.usage, options, true);
            return analysis;
        }

        CommandToken token;
        token.start = pos;
        token.end = match.end;
        if (match.incomplete)
        {
            // A position with words still to type: shown as an argument, and nothing to complete.
            for (int32_t index : options)
            {
                const CommandNode& node = *tree->Node(index);
                if (node.type == CommandNode::Type::Argument && WordsNeeded(node.parser) > 1)
                {
                    token.kind = CommandToken::Kind::Argument;
                    token.parser = node.parser;
                    token.label = Label(node);
                    break;
                }
            }
            analysis.tokens.push_back(token);
            analysis.usage = MatchedUsage(analysis);
            if (!analysis.usage.empty()) analysis.usage.back().current = true;
            return analysis;
        }

        const CommandNode& node = *tree->Node(match.node);
        token.kind = node.type == CommandNode::Type::Literal ? CommandToken::Kind::Literal : CommandToken::Kind::Argument;
        token.parser = node.parser;
        token.label = Label(node);
        analysis.tokens.push_back(token);
        last = &node;

        if (match.end >= input.size()) // The word runs to the end: no space after it yet.
        {
            analysis.executable = node.executable;
            Suggest(options, input.substr(pos, match.end - pos), players, analysis);
            analysis.usage = MatchedUsage(analysis);
            analysis.usage.back().current = true;
            AppendAhead(analysis.usage, tree->Next(node), false);
            return analysis;
        }
        options = tree->Next(node);
        pos = match.end;
    }
}

} // namespace Volcano
