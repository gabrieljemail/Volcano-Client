#pragma once
#ifndef VOLCANO_TEXT_COMPONENT_H
#define VOLCANO_TEXT_COMPONENT_H

#include <cstdint>
#include <string>
#include <vector>
#include "NBT.hpp"
#include "VarInt.hpp"

namespace Volcano {

// One contiguously-styled run of a flattened text component — a
// component's "text"/"extra" tree collapses into an ordered list of these
// so callers (the chat window, debug logging) don't need to walk NBT
// themselves.
struct ChatSegment {
    std::string text;
    uint32_t colorRGB = 0xFFFFFFu; // packed 0xRRGGBB, defaults to white
};

// Flattens a text component already parsed as an NBT::Tag (a String tag for
// plain text, a Compound tag for anything styled, or a List tag for an
// implicit array of components — see the protocol's Text Component
// encoding) into styled runs, resolving each segment's Minecraft color
// name/hex code and inheriting the nearest ancestor's color into children
// that don't override it. "translate" components fall back to showing the
// raw translation key — no lang-file lookup is done.
std::vector<ChatSegment> ParseTextComponent(const NBT::Tag& tag);

// Reads one network-encoded (unnamed — network NBT omits the root tag's
// name) text component directly off `reader` and flattens it the same way
// as ParseTextComponent, leaving `reader` positioned right after it.
std::vector<ChatSegment> ReadTextComponent(PacketReader& reader);

// Concatenates a flattened component's runs back into plain text (styling
// discarded) — for logging or anywhere only the words matter.
std::string PlainText(const std::vector<ChatSegment>& segments);

} // namespace Volcano

#endif
