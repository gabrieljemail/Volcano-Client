#pragma once
#ifndef VOLCANO_GUI_COMMAND_STYLE_H
#define VOLCANO_GUI_COMMAND_STYLE_H

#include <string>
#include <vector>
#include <Pincel/Pincel.hpp>
#include "CommandWalker.hpp"

namespace Volcano::GUI {

// How a command typed in the chat box looks, in the colors of Theme::Syntax. Render thread only.
class CommandStyle {
public:
    // The colored runs of the command's text, as character positions for Pincel's Input.
    std::vector<Pincel::InputHighlight> Highlight(const std::string& text, const CommandAnalysis& analysis) const;

    // The usage line: each part in its color, with the one being typed in bold.
    std::vector<Pincel::TextSpan> UsageSpans(const CommandAnalysis& analysis) const;

    // The literals typed so far, which is what a command's description is looked up by.
    std::vector<std::string> Words(const std::string& text, const CommandAnalysis& analysis) const;
};

} // namespace Volcano::GUI

#endif
