#include "CommandTree.hpp"
#include <array>
#include <stdexcept>

namespace Volcano {

namespace {

constexpr int32_t MAX_NODES = 1 << 20;

constexpr std::array<const char*, static_cast<size_t>(CommandParser::Count)> PARSER_NAMES = {
    "bool", "float", "double", "integer", "long", "string", "entity", "game_profile", "block_pos", "column_pos",
    "vec3", "vec2", "block_state", "block_predicate", "item_stack", "item_predicate", "color", "hex_color",
    "component", "style", "message", "nbt", "nbt_tag", "nbt_path", "objective", "objective_criteria",
    "operation", "particle", "angle", "rotation", "scoreboard_slot", "score_holder", "swizzle", "team",
    "item_slot", "item_slots", "resource_location", "function", "entity_anchor", "int_range", "float_range",
    "dimension", "gamemode", "time", "resource_or_tag", "resource_or_tag_key", "resource", "resource_key",
    "resource_selector", "template_mirror", "template_rotation", "heightmap", "loot_table", "loot_predicate",
    "loot_modifier", "dialog", "uuid",
};

// A number parser's flags byte, then the bounds it says are present.
template<typename Read>
void ReadBounds(PacketReader& reader, CommandNode& node, Read read)
{
    node.flags = reader.ReadByte();
    if (node.flags & 0x01) node.min = read();
    if (node.flags & 0x02) node.max = read();
}

void ReadParserProperties(PacketReader& reader, CommandNode& node)
{
    switch (node.parser)
    {
    case CommandParser::Float: ReadBounds(reader, node, [&] { return static_cast<double>(reader.ReadFloat()); }); break;
    case CommandParser::Double: ReadBounds(reader, node, [&] { return reader.ReadDouble(); }); break;
    case CommandParser::Integer: ReadBounds(reader, node, [&] { return static_cast<double>(reader.ReadInt()); }); break;
    case CommandParser::Long: ReadBounds(reader, node, [&] { return static_cast<double>(reader.ReadLong()); }); break;
    case CommandParser::String: node.stringMode = reader.ReadVarInt(); break;
    case CommandParser::Entity:
    case CommandParser::ScoreHolder: node.flags = reader.ReadByte(); break;
    case CommandParser::Time: node.min = reader.ReadInt(); break;
    case CommandParser::ResourceOrTag:
    case CommandParser::ResourceOrTagKey:
    case CommandParser::Resource:
    case CommandParser::ResourceKey:
    case CommandParser::ResourceSelector: node.registry = reader.ReadString(); break;
    default: break;
    }
}

CommandNode ReadNode(PacketReader& reader)
{
    constexpr uint8_t TYPE_MASK = 0x03, EXECUTABLE = 0x04, HAS_REDIRECT = 0x08, HAS_SUGGESTIONS = 0x10, RESTRICTED = 0x20;

    CommandNode node;
    uint8_t flags = reader.ReadByte();
    if ((flags & TYPE_MASK) > static_cast<uint8_t>(CommandNode::Type::Argument)) throw std::runtime_error("Bad command node type");
    node.type = static_cast<CommandNode::Type>(flags & TYPE_MASK);
    node.executable = (flags & EXECUTABLE) != 0;
    node.restricted = (flags & RESTRICTED) != 0;

    int32_t childCount = reader.ReadVarInt();
    if (childCount < 0 || static_cast<size_t>(childCount) > reader.Remaining()) throw std::runtime_error("Bad command child count");
    node.children.reserve(static_cast<size_t>(childCount));
    for (int32_t i = 0; i < childCount; i++) node.children.push_back(reader.ReadVarInt());

    if (flags & HAS_REDIRECT) node.redirect = reader.ReadVarInt();

    if (node.type != CommandNode::Type::Root) node.name = reader.ReadString();
    if (node.type == CommandNode::Type::Argument)
    {
        int32_t parser = reader.ReadVarInt();
        if (parser < 0 || parser >= static_cast<int32_t>(CommandParser::Count))
        {
            throw std::runtime_error("Unknown command parser id " + std::to_string(parser));
        }
        node.parser = static_cast<CommandParser>(parser);
        ReadParserProperties(reader, node);
        if (flags & HAS_SUGGESTIONS) node.suggestions = reader.ReadString();
    }
    return node;
}

} // namespace

std::shared_ptr<const CommandTree> CommandTree::Read(PacketReader& reader)
{
    int32_t count = reader.ReadVarInt();
    if (count <= 0 || count > MAX_NODES) throw std::runtime_error("Bad command node count");

    auto tree = std::make_shared<CommandTree>();
    tree->nodes.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; i++) tree->nodes.push_back(ReadNode(reader));
    tree->rootIndex = reader.ReadVarInt();

    // Every index must land in the tree, so walking it never needs a check.
    auto valid = [count](int32_t index) { return index >= 0 && index < count; };
    if (!valid(tree->rootIndex)) throw std::runtime_error("Bad command root index");
    for (const CommandNode& node : tree->nodes)
    {
        if (node.redirect != -1 && !valid(node.redirect)) throw std::runtime_error("Bad command redirect");
        for (int32_t child : node.children)
        {
            if (!valid(child)) throw std::runtime_error("Bad command child index");
        }
    }
    return tree;
}

const CommandNode* CommandTree::Node(int32_t index) const
{
    return index >= 0 && static_cast<size_t>(index) < nodes.size() ? &nodes[static_cast<size_t>(index)] : nullptr;
}

const std::vector<int32_t>& CommandTree::Next(const CommandNode& node) const
{
    return node.redirect >= 0 ? nodes[static_cast<size_t>(node.redirect)].children : node.children;
}

const char* CommandTree::ParserName(CommandParser parser)
{
    size_t index = static_cast<size_t>(parser);
    return index < PARSER_NAMES.size() ? PARSER_NAMES[index] : "unknown";
}

} // namespace Volcano
