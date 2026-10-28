#include "SlotSkipper.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace Volcano {

namespace {

constexpr const char* PROTOCOL_JSON_PATH = "resources/minecraft-data/data/pc/26.1/protocol.json";
constexpr int MAX_DEPTH = 64;

// A decoded scalar kept for sibling-field lookups (switch compareTo, array count).
struct Value {
    enum class Kind { None, Int, Bool, Str };
    Kind kind = Kind::None;
    int64_t number = 0;
    std::string text;

    static Value Int(int64_t n) { Value v; v.kind = Kind::Int; v.number = n; return v; }
    static Value Bool(bool b) { Value v; v.kind = Kind::Bool; v.number = b ? 1 : 0; return v; }
    static Value Str(std::string s) { Value v; v.kind = Kind::Str; v.text = std::move(s); return v; }

    // Key form used by protodef switch "fields" maps.
    std::string Key() const
    {
        switch (kind)
        {
        case Kind::Int: return std::to_string(number);
        case Kind::Bool: return number ? "true" : "false";
        case Kind::Str: return text;
        default: throw std::runtime_error("SlotSkipper: switch on a value that was not stored");
        }
    }
};

struct Scope {
    std::unordered_map<std::string, Value> vars;
    Scope* parent = nullptr;
};

[[noreturn]] void Fail(const std::string& why)
{
    throw std::runtime_error("SlotSkipper: " + why);
}

// Network NBT payload (no names on the root; names inside compounds).
void SkipNbtPayload(PacketReader& reader, uint8_t type, int depth)
{
    if (depth > 128) Fail("NBT nested too deep");
    auto skipCounted = [&](size_t elementSize)
    {
        int32_t length = reader.ReadInt();
        if (length < 0) Fail("negative NBT array length");
        reader.Skip(static_cast<size_t>(length) * elementSize);
    };

    switch (type)
    {
    case 1: reader.Skip(1); break;
    case 2: reader.Skip(2); break;
    case 3: reader.Skip(4); break;
    case 4: reader.Skip(8); break;
    case 5: reader.Skip(4); break;
    case 6: reader.Skip(8); break;
    case 7: skipCounted(1); break;
    case 8: reader.Skip(reader.ReadShort() & 0xFFFF); break;
    case 9:
    {
        uint8_t elementType = reader.ReadByte();
        int32_t length = reader.ReadInt();
        if (length < 0) Fail("negative NBT list length");
        if (elementType == 0 && length > 0) Fail("NBT list of End");
        for (int32_t i = 0; i < length; i++) SkipNbtPayload(reader, elementType, depth + 1);
        break;
    }
    case 10:
        for (;;)
        {
            uint8_t childType = reader.ReadByte();
            if (childType == 0) break;
            reader.Skip(reader.ReadShort() & 0xFFFF);
            SkipNbtPayload(reader, childType, depth + 1);
        }
        break;
    case 11: skipCounted(4); break;
    case 12: skipCounted(8); break;
    default: Fail("bad NBT tag type");
    }
}

// Root tag of anonymousNbt: a type byte then an unnamed payload. A bare
// TAG_End (0) is an empty tag.
void SkipAnonymousNbt(PacketReader& reader)
{
    uint8_t type = reader.ReadByte();
    if (type != 0) SkipNbtPayload(reader, type, 0);
}

void SkipLpVec3(PacketReader& reader)
{
    uint8_t lowest = reader.ReadByte();
    if (lowest == 0) return;
    reader.Skip(5); // middle byte + 4 bytes
    if ((lowest & 4u) != 0) reader.ReadVarInt();
}

Value ReadFixedInt(PacketReader& reader, int bytes, bool isSigned)
{
    uint64_t raw = 0;
    for (int i = 0; i < bytes; i++) raw = (raw << 8) | reader.ReadByte();
    int64_t value = static_cast<int64_t>(raw);
    if (isSigned && bytes < 8)
    {
        uint64_t signBit = 1ull << (bytes * 8 - 1);
        if (raw & signBit) value = static_cast<int64_t>(raw) - static_cast<int64_t>(signBit << 1);
    }
    return Value::Int(value);
}

} // namespace

struct SlotSkipper::Impl {
    json types;
    std::unordered_map<int32_t, std::string> componentNames;
    std::unordered_map<std::string, const json*> componentTypes;
    bool loaded = false;

    bool Load(const std::string& path)
    {
        try
        {
            std::ifstream file(path);
            if (!file) return false;
            json root;
            file >> root;
            types = std::move(root.at("types"));

            for (auto& [key, name] : types.at("SlotComponentType").at(1).at("mappings").items())
            {
                componentNames[std::stoi(key)] = name.get<std::string>();
            }

            // SlotComponent = container [type, data: switch(type)].
            const json& dataSwitch = types.at("SlotComponent").at(1).at(1).at("type").at(1);
            for (auto& [name, def] : dataSwitch.at("fields").items())
            {
                componentTypes[name] = &def;
            }
            loaded = true;
        }
        catch (const std::exception&)
        {
            loaded = false;
        }
        return loaded;
    }

    const Value& Lookup(Scope& scope, const std::string& path) const
    {
        Scope* current = &scope;
        size_t start = 0;
        for (;;)
        {
            size_t slash = path.find('/', start);
            std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (slash != std::string::npos)
            {
                if (part != "..") Fail("unsupported compareTo path " + path);
                current = current->parent;
                if (!current) Fail("compareTo went above the root scope");
                start = slash + 1;
                continue;
            }
            if (part == "..") Fail("compareTo path ends in ..");
            auto it = current->vars.find(part);
            if (it == current->vars.end()) Fail("unknown field " + part);
            return it->second;
        }
    }

    size_t ReadCount(const json& params, PacketReader& reader, Scope& scope, int depth) const
    {
        int64_t count;
        if (params.contains("count"))
        {
            const json& c = params["count"];
            count = c.is_number_integer() ? c.get<int64_t>() : Lookup(scope, c.get<std::string>()).number;
        }
        else
        {
            count = Skip(params.at("countType"), reader, scope, false, depth + 1).number;
        }
        // Every real element takes at least a byte, so a count beyond what is
        // left can only be corrupt data.
        if (count < 0 || static_cast<size_t>(count) > reader.Remaining()) Fail("implausible count");
        return static_cast<size_t>(count);
    }

    Value SkipNative(const std::string& name, PacketReader& reader) const
    {
        if (name == "varint" || name == "optvarint") return Value::Int(reader.ReadVarInt());
        if (name == "varlong") return Value::Int(reader.ReadVarLong());
        if (name == "u8") return ReadFixedInt(reader, 1, false);
        if (name == "i8") return ReadFixedInt(reader, 1, true);
        if (name == "u16") return ReadFixedInt(reader, 2, false);
        if (name == "i16") return ReadFixedInt(reader, 2, true);
        if (name == "u32") return ReadFixedInt(reader, 4, false);
        if (name == "i32") return ReadFixedInt(reader, 4, true);
        if (name == "u64" || name == "i64") { reader.Skip(8); return {}; }
        if (name == "bool") return Value::Bool(reader.ReadBool());
        if (name == "f32") { reader.Skip(4); return {}; }
        if (name == "f64") { reader.Skip(8); return {}; }
        if (name == "UUID") { reader.Skip(16); return {}; }
        if (name == "void") return {};
        if (name == "restBuffer") { reader.Skip(reader.Remaining()); return {}; }
        if (name == "anonymousNbt" || name == "anonOptionalNbt") { SkipAnonymousNbt(reader); return {}; }
        if (name == "lpVec3") { SkipLpVec3(reader); return {}; }
        Fail("unsupported native type " + name);
    }

    // Skips one value of `type`. `flatten` makes a container write its fields
    // into `scope` instead of a child scope (protodef anon fields).
    Value Skip(const json& type, PacketReader& reader, Scope& scope, bool flatten, int depth) const
    {
        if (depth > MAX_DEPTH) Fail("type nesting too deep");

        if (type.is_string())
        {
            const std::string& name = type.get_ref<const std::string&>();
            auto it = types.find(name);
            if (it == types.end()) Fail("unknown type " + name);
            if (it->is_string() && it->get_ref<const std::string&>() == "native") return SkipNative(name, reader);
            return Skip(*it, reader, scope, flatten, depth + 1);
        }
        if (!type.is_array() || type.size() < 1) Fail("malformed type definition");

        const std::string& kind = type[0].get_ref<const std::string&>();
        static const json noParams;
        const json& params = type.size() > 1 ? type[1] : noParams;

        if (kind == "container")
        {
            Scope child;
            child.parent = &scope;
            Scope& local = flatten ? scope : child;
            for (const json& field : params)
            {
                bool anon = field.value("anon", false);
                Value v = Skip(field.at("type"), reader, local, anon, depth + 1);
                if (!anon && field.contains("name") && v.kind != Value::Kind::None)
                {
                    local.vars[field["name"].get<std::string>()] = std::move(v);
                }
            }
            return {};
        }
        if (kind == "array")
        {
            size_t count = ReadCount(params, reader, scope, depth);
            const json& element = params.at("type");
            for (size_t i = 0; i < count; i++)
            {
                Scope elementScope;
                elementScope.parent = &scope;
                Skip(element, reader, elementScope, false, depth + 1);
            }
            return {};
        }
        if (kind == "pstring" || kind == "buffer")
        {
            reader.Skip(ReadCount(params, reader, scope, depth));
            return {};
        }
        if (kind == "switch")
        {
            if (params.contains("compareToValue")) Fail("compareToValue is unsupported");
            std::string key = Lookup(scope, params.at("compareTo").get<std::string>()).Key();
            const json& fields = params.at("fields");
            auto it = fields.find(key);
            if (it != fields.end()) return Skip(*it, reader, scope, flatten, depth + 1);
            if (params.contains("default")) return Skip(params["default"], reader, scope, flatten, depth + 1);
            return {};
        }
        if (kind == "option")
        {
            if (reader.ReadBool()) Skip(params, reader, scope, false, depth + 1);
            return {};
        }
        if (kind == "mapper")
        {
            Value raw = Skip(params.at("type"), reader, scope, false, depth + 1);
            auto it = params.at("mappings").find(std::to_string(raw.number));
            if (it != params.at("mappings").end()) return Value::Str(it->get<std::string>());
            return raw;
        }
        if (kind == "registryEntryHolder")
        {
            // 0 = inline value follows; otherwise it is a registry id + 1.
            if (reader.ReadVarInt() == 0) Skip(params.at("otherwise").at("type"), reader, scope, false, depth + 1);
            return {};
        }
        if (kind == "registryEntryHolderSet")
        {
            // 0 = tag name follows; otherwise (count + 1) entries.
            int32_t n = reader.ReadVarInt();
            if (n < 0) Fail("negative holder set size");
            if (n == 0)
            {
                Skip(params.at("base").at("type"), reader, scope, false, depth + 1);
            }
            else
            {
                if (static_cast<size_t>(n - 1) > reader.Remaining()) Fail("implausible holder set size");
                for (int32_t i = 0; i < n - 1; i++) Skip(params.at("otherwise").at("type"), reader, scope, false, depth + 1);
            }
            return {};
        }
        if (kind == "bitfield")
        {
            int bits = 0;
            for (const json& field : params) bits += field.at("size").get<int>();
            reader.Skip(static_cast<size_t>(bits / 8));
            return {};
        }
        Fail("unsupported type kind " + kind);
    }

    bool SkipComponents(PacketReader& reader, int32_t addedCount, int32_t removedCount, SlotComponentValues& out) const
    {
        if (!loaded || addedCount < 0 || removedCount < 0) return false;
        try
        {
            for (int32_t i = 0; i < addedCount; i++)
            {
                int32_t id = reader.ReadVarInt();
                auto nameIt = componentNames.find(id);
                if (nameIt == componentNames.end()) return false;
                const std::string& name = nameIt->second;

                if (name == "damage") { out.damage = reader.ReadVarInt(); continue; }
                if (name == "max_damage") { out.maxDamage = reader.ReadVarInt(); continue; }

                auto defIt = componentTypes.find(name);
                if (defIt == componentTypes.end()) continue; // No switch entry means void.
                Scope scope;
                Skip(*defIt->second, reader, scope, false, 0);
            }
            for (int32_t i = 0; i < removedCount; i++) reader.ReadVarInt();
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
};

SlotSkipper::SlotSkipper(const std::string& path) : impl(std::make_unique<Impl>())
{
    impl->Load(path);
}

SlotSkipper::~SlotSkipper() = default;

SlotSkipper& SlotSkipper::Instance()
{
    static SlotSkipper instance(PROTOCOL_JSON_PATH);
    return instance;
}

bool SlotSkipper::IsLoaded() const
{
    return impl->loaded;
}

bool SlotSkipper::SkipComponents(PacketReader& reader, int32_t addedCount, int32_t removedCount, SlotComponentValues& out) const
{
    return impl->SkipComponents(reader, addedCount, removedCount, out);
}

} // namespace Volcano
