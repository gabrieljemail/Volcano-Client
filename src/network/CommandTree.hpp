#pragma once
#ifndef VOLCANO_COMMAND_TREE_H
#define VOLCANO_COMMAND_TREE_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "VarInt.hpp"

namespace Volcano {

// Argument parser ids of the Commands packet, in protocol order (26.1).
enum class CommandParser : int32_t {
    Bool, Float, Double, Integer, Long, String, Entity, GameProfile, BlockPos, ColumnPos, Vec3, Vec2,
    BlockState, BlockPredicate, ItemStack, ItemPredicate, Color, HexColor, Component, Style, Message,
    Nbt, NbtTag, NbtPath, Objective, ObjectiveCriteria, Operation, Particle, Angle, Rotation,
    ScoreboardSlot, ScoreHolder, Swizzle, Team, ItemSlot, ItemSlots, ResourceLocation, Function,
    EntityAnchor, IntRange, FloatRange, Dimension, GameMode, Time, ResourceOrTag, ResourceOrTagKey,
    Resource, ResourceKey, ResourceSelector, TemplateMirror, TemplateRotation, Heightmap, LootTable,
    LootPredicate, LootModifier, Dialog, Uuid,
    Count
};

struct CommandNode {
    enum class Type : uint8_t { Root, Literal, Argument };

    Type type = Type::Root;
    bool executable = false;                // The command can end here.
    bool restricted = false;                // Needs permission the server checks.
    std::string name;                       // The literal's text, or the argument's name.
    std::vector<int32_t> children;
    int32_t redirect = -1;                  // The node whose children continue this one, or -1.

    // Arguments only.
    CommandParser parser = CommandParser::Bool;
    uint8_t flags = 0;                      // Entity: 1 single, 2 players only. Score holder: 1 multiple.
    int32_t stringMode = 0;                 // 0 word, 1 quotable phrase, 2 greedy phrase.
    std::optional<double> min;              // Number bounds, or a time's minimum.
    std::optional<double> max;
    std::string registry;                   // For the resource parsers.
    std::string suggestions;                // Like minecraft:ask_server; empty when the parser lists its own.
};

// The server's Brigadier command tree, as sent in the Commands packet.
// Immutable once read, so it is shared by pointer: see GlobalState::commands.
class CommandTree {
public:
    // Reads a Commands packet body. Throws if it is malformed or has a parser this client doesn't know.
    static std::shared_ptr<const CommandTree> Read(PacketReader& reader);

    const CommandNode& Root() const { return nodes[static_cast<size_t>(rootIndex)]; }
    const CommandNode* Node(int32_t index) const;
    size_t Size() const { return nodes.size(); }

    // The nodes input continues into after `node`: its redirect's children, if it has a redirect.
    const std::vector<int32_t>& Next(const CommandNode& node) const;

    static const char* ParserName(CommandParser parser);

private:
    std::vector<CommandNode> nodes;
    int32_t rootIndex = 0;
};

} // namespace Volcano

#endif
