#pragma once
#ifndef VOLCANO_NBT_H
#define VOLCANO_NBT_H

#include <cstdint>
#include <istream>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace Volcano::NBT {

struct Tag;

enum class TagType : uint8_t {
    End = 0, Byte = 1, Short = 2, Int = 3, Long = 4, Float = 5,
    Double = 6, ByteArray = 7, String = 8, List = 9, Compound = 10, IntArray = 11, LongArray = 12
};

using TagCompound = std::map<std::string, Tag>;

struct TagList {
    TagType elementType = TagType::End;
    std::vector<Tag> elements;
};

using TagValue = std::variant<
    std::monostate,                    // TagType::End
    int8_t,                            // TagType::Byte
    int16_t,                           // TagType::Short
    int32_t,                           // TagType::Int
    int64_t,                           // TagType::Long
    float,                             // TagType::Float
    double,                            // TagType::Double
    std::vector<int8_t>,               // TagType::ByteArray
    std::string,                       // TagType::String
    std::unique_ptr<TagList>,          // TagType::List
    std::unique_ptr<TagCompound>,      // TagType::Compound
    std::vector<int32_t>,              // TagType::IntArray
    std::vector<int64_t>               // TagType::LongArray
>;

struct Tag {
    TagType type = TagType::End;
    TagValue value;

    Tag() : type(TagType::End), value(std::monostate{}) {}
    Tag(TagType t, TagValue v) : type(t), value(std::move(v)) {}
};

// Reads one full named NBT tag (type byte + name + payload) from `stream`,
// e.g. the root compound of an uncompressed .dat/.nbt blob. `outRootName`
// receives the root tag's name.
Tag Parse(std::istream& stream, std::string& outRootName);

TagValue ReadPayload(std::istream& stream, TagType type);

// Writes one named tag (type byte + name + payload) — or just the payload,
// if `named` is false (used recursively for list elements, which are
// unnamed).
void WriteTag(std::ostream& stream, const std::string& name, const Tag& tag, bool named = true);

} // namespace Volcano::NBT

#endif
