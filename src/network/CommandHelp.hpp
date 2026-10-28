#pragma once
#ifndef VOLCANO_COMMAND_HELP_H
#define VOLCANO_COMMAND_HELP_H

#include <string>
#include <unordered_map>
#include <vector>

namespace Volcano {

// What each command does, in a sentence, for the chat box's usage tooltip. The server's command tree has no
// descriptions, so they come from resources/commands.json: an object from a command ("data") or a subcommand
// path ("data modify") to its description. The longest path that matches wins.
class CommandHelp {
public:
    // Reads the file. Returns false, leaving it empty, if it is missing or not an object of strings.
    bool Load(const std::string& path);

    // `words` are the literals typed so far, in order. Empty if there is nothing for them.
    const std::string& Describe(const std::vector<std::string>& words) const;

private:
    std::unordered_map<std::string, std::string> descriptions;
};

} // namespace Volcano

#endif
