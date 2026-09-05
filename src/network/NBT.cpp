#include "NBT.hpp"
#include <algorithm>
#include <bit>
#include <concepts>
#include <type_traits>

namespace Volcano::NBT {

namespace {

template <std::integral T>
T ReadBigEndian(std::istream& stream)
{
    T val;
    stream.read(reinterpret_cast<char*>(&val), sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        auto* bytes = reinterpret_cast<uint8_t*>(&val);
        std::reverse(bytes, bytes + sizeof(T));
    }
    return val;
}

template <typename T>
void WriteBigEndian(std::ostream& stream, T val)
{
    char* ptr = reinterpret_cast<char*>(&val);
    for (int i = sizeof(T) - 1; i >= 0; --i)
    {
        stream.put(ptr[i]);
    }
}

std::string ReadString(std::istream& stream)
{
    uint16_t length = ReadBigEndian<uint16_t>(stream);
    std::string str(length, '\0');
    stream.read(str.data(), length);
    return str;
}

void WriteString(std::ostream& stream, const std::string& str)
{
    uint16_t len = static_cast<uint16_t>(str.size());
    WriteBigEndian<uint16_t>(stream, len);
    stream.write(str.data(), len);
}

} // namespace

TagValue ReadPayload(std::istream& stream, TagType type)
{
    switch (type) {
        case TagType::End: return std::monostate{};
        case TagType::Byte: return ReadBigEndian<int8_t>(stream);
        case TagType::Short: return ReadBigEndian<int16_t>(stream);
        case TagType::Int: return ReadBigEndian<int32_t>(stream);
        case TagType::Long: return ReadBigEndian<int64_t>(stream);
        case TagType::Float: {
            int32_t bits = ReadBigEndian<int32_t>(stream);
            return std::bit_cast<float>(bits);
        }
        case TagType::Double: {
            int64_t bits = ReadBigEndian<int64_t>(stream);
            return std::bit_cast<double>(bits);
        }
        case TagType::ByteArray: {
            int32_t size = ReadBigEndian<int32_t>(stream);
            std::vector<int8_t> arr(size);
            stream.read(reinterpret_cast<char*>(arr.data()), size);
            return arr;
        }
        case TagType::String: return ReadString(stream);
        case TagType::List: {
            auto list = std::make_unique<TagList>();
            list->elementType = static_cast<TagType>(ReadBigEndian<uint8_t>(stream));
            int32_t size = ReadBigEndian<int32_t>(stream);
            list->elements.reserve(size);

            for (int32_t i = 0; i < size; ++i)
            {
                list->elements.push_back(Tag{list->elementType, ReadPayload(stream, list->elementType)});
            }

            return list;
        }
        case TagType::Compound: {
            auto compound = std::make_unique<TagCompound>();

            while (true)
            {
                auto innerType = static_cast<TagType>(ReadBigEndian<uint8_t>(stream));
                if (innerType == TagType::End) break;
                std::string name = ReadString(stream);
                (*compound)[name] = Tag{innerType, ReadPayload(stream, innerType)};
            }
            return compound;
        }
        case TagType::IntArray: {
            int32_t size = ReadBigEndian<int32_t>(stream);
            std::vector<int32_t> arr(size);

            for (int32_t i = 0; i < size; ++i)
            {
                arr[i] = ReadBigEndian<int32_t>(stream);
            }
            return arr;
        }
        case TagType::LongArray: {
            int32_t size = ReadBigEndian<int32_t>(stream);
            std::vector<int64_t> arr(size);

            for (int32_t i = 0; i < size; ++i)
            {
                arr[i] = ReadBigEndian<int64_t>(stream);
            }
            return arr;
        }
    }
    return std::monostate{};
}

Tag Parse(std::istream& stream, std::string& outRootName)
{
    auto rootType = static_cast<TagType>(ReadBigEndian<uint8_t>(stream));
    if (rootType == TagType::End) return {};
    outRootName = ReadString(stream);
    return Tag{rootType, ReadPayload(stream, rootType)};
}

void WriteTag(std::ostream& stream, const std::string& name, const Tag& tag, bool named)
{
    if (named)
    {
        stream.put(static_cast<char>(tag.type));
        WriteString(stream, name);
    }

    std::visit([&stream](const auto& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>)
        {
            // End tag: no payload.
        } else if constexpr (std::is_same_v<T, int8_t>)
        {
            stream.put(static_cast<char>(arg));
        } else if constexpr (std::is_same_v<T, int16_t>
                             || std::is_same_v<T, int32_t>
                             || std::is_same_v<T, int64_t>)
        {
            WriteBigEndian(stream, arg);
        } else if constexpr (std::is_same_v<T, float>)
        {
            WriteBigEndian(stream, std::bit_cast<int32_t>(arg));
        } else if constexpr (std::is_same_v<T, double>)
        {
            WriteBigEndian(stream, std::bit_cast<int64_t>(arg));
        } else if constexpr (std::is_same_v<T, std::string>)
        {
            WriteString(stream, arg);
        } else if constexpr (std::is_same_v<T, std::vector<int8_t>>)
        {
            WriteBigEndian<int32_t>(stream, static_cast<int32_t>(arg.size()));
            stream.write(reinterpret_cast<const char*>(arg.data()), static_cast<std::streamsize>(arg.size()));
        } else if constexpr (std::is_same_v<T, std::vector<int32_t>>)
        {
            WriteBigEndian<int32_t>(stream, static_cast<int32_t>(arg.size()));
            for (int32_t v : arg) WriteBigEndian(stream, v);
        } else if constexpr (std::is_same_v<T, std::vector<int64_t>>)
        {
            WriteBigEndian<int32_t>(stream, static_cast<int32_t>(arg.size()));
            for (int64_t v : arg) WriteBigEndian(stream, v);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<TagCompound>>)
        {
            for (const auto& [childName, childTag] : *arg)
            {
                WriteTag(stream, childName, childTag, true);
            }
            stream.put(static_cast<char>(TagType::End));
        } else if constexpr (std::is_same_v<T, std::unique_ptr<TagList>>)
        {
            stream.put(static_cast<char>(arg->elementType));
            WriteBigEndian<int32_t>(stream, static_cast<int32_t>(arg->elements.size()));
            for (const Tag& element : arg->elements)
            {
                WriteTag(stream, "", element, false);
            }
        }
    }, tag.value);
}

} // namespace Volcano::NBT
