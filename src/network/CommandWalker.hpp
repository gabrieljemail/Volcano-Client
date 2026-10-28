#pragma once
#ifndef VOLCANO_COMMAND_WALKER_H
#define VOLCANO_COMMAND_WALKER_H

#include <memory>
#include <string>
#include <vector>
#include "CommandTree.hpp"

namespace Volcano {

struct CommandToken {
    enum class Kind : uint8_t {
        Literal,    // A word the tree has.
        Argument,   // A value that fits its argument; see `parser`.
        Partial,    // The word being typed, which could still become valid.
        Invalid     // Nothing at this point accepts it, or anything after it.
    };

    Kind kind = Kind::Invalid;
    CommandParser parser = CommandParser::Bool;    // Arguments only.
    std::string label;                             // The usage form of what it matched, empty if nothing did.
    size_t start = 0;
    size_t end = 0;
};

// One piece of the usage line: what the command looks like, as typed so far and as it goes on.
struct CommandUsagePart {
    std::string text;           // A literal, or an argument like <Entity target[1]>, or (a | b) for a choice.
    bool argument = false;
    bool choice = false;        // Several things can come here.
    bool current = false;       // The word being typed, or the one expected next.
};

struct CommandSuggestion {
    std::string text;           // Replaces the word being typed. Empty for a hint.
    std::string label;          // What is shown: the text, or <name> for a hint.
    bool hint = false;          // An argument that has no value to offer, only its name.
    bool more = false;          // Input continues after it, so completing adds a space.
    bool literal = false;       // A word of the command itself, not a value for an argument.
};

struct CommandAnalysis {
    std::vector<CommandToken> tokens;
    bool executable = false;                    // What is typed is a whole command the server can run.
    bool failed = false;                        // A word matched nothing.
    size_t partialStart = 0;                    // Where the word being typed starts.
    std::vector<CommandSuggestion> suggestions; // For that word: literals, then values, then hints.
    std::vector<CommandUsagePart> usage;        // The words matched, then the way on while it is the only way.
};

// Matches typed input against a command tree, the way Brigadier does, for the chat box's syntax coloring
// and Tab completion. Input is the command without its "/". Parsers that take structured text (NBT, item
// components, ranges) are only checked for being a word, so the server remains the one that rejects them.
class CommandWalker {
public:
    explicit CommandWalker(std::shared_ptr<const CommandTree> commandTree) : tree(std::move(commandTree)) {}

    // `players` are the names offered for target arguments, sorted.
    CommandAnalysis Analyze(const std::string& input, const std::vector<std::string>& players) const;

private:
    struct Match {
        int32_t node = -1;
        size_t end = 0;
        bool incomplete = false;    // A multi-word argument that ran out of words.
    };

    Match TryArgument(const CommandNode& node, int32_t index, const std::string& input, size_t start) const;
    Match Choose(const std::vector<int32_t>& options, const std::string& input, size_t start) const;
    bool CouldFit(const std::vector<int32_t>& options, const std::string& word) const;
    void Suggest(const std::vector<int32_t>& options, const std::string& partial,
        const std::vector<std::string>& players, CommandAnalysis& analysis) const;
    std::string Label(const CommandNode& node) const;
    void AppendAhead(std::vector<CommandUsagePart>& usage, std::vector<int32_t> options, bool firstIsCurrent) const;

    std::shared_ptr<const CommandTree> tree;
};

} // namespace Volcano

#endif
