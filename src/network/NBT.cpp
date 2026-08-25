/* 
 * Volcano Client NBT data structure models.
 * Written by Gabriel Jemail <gabriel@jemail.us>
 */

/*
#pragma once
#ifndef VOLCANO_NBT_H
#define VOLCANO_NBT_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <map>
#include <string>
#include <variant>
#include <memory>
#include <iostream>
#include <bit>
#include <concepts>

using namespace std;

namespace Volcano::NBT {

#pragma region Types

struct Tag;

enum class TagType : uint8_t {
    End = 0, Byte = 1, Short = 2, Int = 3, Long = 4, Float = 5,
    Double = 6, ByteArray = 7, String = 8, List = 9, Compound = 10, IntArray = 11, LongArray = 12
};

using TagCompound = map<string, Tag>;

struct TagList {
    TagType type = TagType::End;
    vector<Tag> elements;
};

using TagValue = variant<
    monostate,                  // TagType::End
    int8_t,                     // TagType::Byte
    int16_t,                    // TagType::Short
    int32_t,                    // TagType::Int
    int64_t,                    // TagType::Long
    float,                      // TagType::Float
    double,                     // TagType::Double
    vector<int8_t>              // TagType::ByteArray
    string,                     // TagType::String
    unique_ptr<TagList>,        // TagType::List
    unique_ptr<TagCompound>,    // TagType::Compound
    vector<int32_t>,            // TagType::IntArray
    vector<int64_t>             // TagType::LongArray
>;

struct Tag {
    TagType type = TagType::End;
    TagValue value;

    Tag() : type(TagType::End), value(monostate{}) {}
    Tag(TagType t, TagValue v) : type(t), value(move(v)) {}
};

#pragma endregion

#pragma region Decoding

template<integral T>
T ReadBigEndian(istream& stream)
{
    T val;
    stream.read(reinterpret_cast<char*>(&val), sizeof(T));
    if constexpr (endian::native == endian::little) {
        auto* bytes = reinterpret_cast<uint8_t*>(&val);
        reverse(bytes, bytes + sizeof(T));
    }
    return val;
}

TagValue ReadPayload(istream& stream, TagType type);

string ReadString(istream& stream) {
    uint16_t length = ReadBigEndian<uint16_t>(stream);
    string str(length, '\0');
    stream.read(&str[0], length);
    return str;
}

TagValue ReadPayload(istream& stream, TagType type) {
    switch (type) {
        case TagType::End: return monostate{};
        case TagType::Byte: return ReadBigEndian<int8_t>(stream);
        case TagType::Short: return ReadBigEndian<int16_t>(stream);
        case TagType::Int: return ReadBigEndian<int32_t>(stream);
        case TagType::Long: return ReadBigEndian<int64_t>(stream);
        case TagType::Float: {
            int32_t bits = ReadBigEndian<int32_t>(stream);
            return bit_cast<float>(bits);
        }
        case TagType::Double: {
            int64_t bits = ReadBigEndian<int64_t>(stream);
            return bit_cast<double>(bits);
        }
        case TagType::ByteArray: {
            int32_t size = ReadBigEndian<int32_t>(stream);
            vector<int8_t> arr(size);
            stream.read(reinterpret_cast<char*>(arr.data()), size);
            return arr;
        }
        case TagType::String: return ReadString(stream);
        case TagType::List: {
            auto list = make_unique<TagList>(ReadBigEndian<uint8_t>(stream));
            list->element_type = static_cast<TagType>(ReadBigEndian<uint8_t>(stream));
            int32_t size = ReadBigEndian<int32_t>(stream);
            list->elements.reserve(size);

            for (int32_t i = 0; i < size; ++i)
            {
                list->elements.push_back(Tag{list->element_type, ReadPayload(stream, list->element_type)});
            }

            return list;
        }
        case TagType::Compound: {
            auto compound = make_unique<TagCompound>();

            while (true)
            {
                auto inner_type = static_cast<TagType>(ReadBigEndian<uint8_t>(stream));
                if (inner_type = TagType::End) break;
                string name = ReadString(stream);
                (*compound)[name] = Tag{inner_type, ReadPayload(stream, inner_type)};
            }
            return compound;
        }
        case TagType::IntArray: {
            int32_t size = ReadBigEndian<int32_t>(stream);
            vector<int32_t> arr(size);

            for (int32_t i = 0; i < size; ++i)
            {
                arr[i] = ReadBigEndian<int32_t>(stream);
            }
            return arr;
        }
        case TagType::LongArray: {
            int32_t size = ReadBigEndian<int32_t>(stream);
            vector<int64_t> arr(size);

            for (int32_t i = 0; i < size; ++i)
            {
                arr[i] = ReadBigEndian<int64_t>(stream);
            }
            return arr;
        }
    }
    return monostate{};
}

Tag Parse(istream& stream, string& outRootName) {
    auto root_type = static_cast<TagType>(ReadBigEndian<uint8_t>(stream));
    if (root_type == TagType::End) return {};
    outRootName = ReadString(stream);
    return Tag{root_type, ReadPayload(stream, root_type)};
}

#pragma endregion

#pragma region Encoding

template <typename T>
void WriteBigEndian(ostream& stream, T val)
{
    char* ptr = reinterpret_cast<char*>(&val);
    for (int i = sizeof(T) - 1; i >= 0; --i)
    {
        stream.put(ptr[i]);
    }
}

void WriteString(ostream& stream, const string& str)
{
    uint16_t len = static_cast<uint16_t>(str.size());
    WriteBigEndian<uint16_t>(stream, len);
    stream.write(str.data(), len);
}

void WriteTag(ostream& stream, const string& name, const Tag& tag, bool named = true)
{
    TagType type = tag->type;
    if (named)
    {
        stream.put(static_cast<char>(type));
        WriteString(stream, name);
    }

    visit([&stream](const auto& arg) {
        using T = decay_t<decltype(arg)>;
        if constexpr (is_same_v<T, int8_t>)
        {
            stream.put(arg);
        } else if constexpr (is_same_v<T, int16_t>
                             || is_same_v<T, int32_t>
                             || is_same_v<T, int64_t>
                             || is_same_v<T, float>
                             || is_same_v<T, double>)
        {
            WriteBigEndian(stream, arg);
        } else if constexpr (is_same_v<T, string>)
        {
            WriteString(stream, arg);
        } else if constexpr (is_same_v<T, TagCompound>)
        {
            for (const auto& [child_name, child_tag] : arg)
            {
                WriteTag(stream, child_name, child_tag, true);
            }
            stream.put(static_cast<char>(TagType::End));
        } else if constexpr (is_same_v<T, TagList>)
        {
            stream.put();
        }
    });
}

#pragma endregion

} // namespace Volcano::NBT

#endif
*/