#include "ZipArchive.hpp"
#include "Logger.hpp"
#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace Volcano {

namespace {

constexpr uint32_t EndOfCentralDirSig = 0x06054b50;
constexpr uint32_t CentralHeaderSig = 0x02014b50;
constexpr uint32_t LocalHeaderSig = 0x04034b50;

uint16_t U16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t U32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

std::unique_ptr<ZipArchive> ZipArchive::Open(const std::string& path)
{
    std::unique_ptr<ZipArchive> archive(new ZipArchive());
    archive->file.open(path, std::ios::binary);
    if (!archive->file.is_open())
    {
        Log::Error("[RESOURCES] Couldn't open zip: " + path);
        return nullptr;
    }
    if (!archive->Index(path)) return nullptr;
    return archive;
}

bool ZipArchive::Index(const std::string& path)
{
    file.seekg(0, std::ios::end);
    std::streamoff fileSize = file.tellg();
    if (fileSize < 22)
    {
        Log::Error("[RESOURCES] Not a zip (too small): " + path);
        return false;
    }

    // The EOCD record sits in the last 22 + 65535 (comment) bytes.
    std::streamoff tailSize = std::min<std::streamoff>(fileSize, 22 + 65535);
    std::vector<uint8_t> tail(static_cast<size_t>(tailSize));
    file.seekg(fileSize - tailSize);
    file.read(reinterpret_cast<char*>(tail.data()), tailSize);
    if (!file)
    {
        Log::Error("[RESOURCES] Failed reading zip tail: " + path);
        return false;
    }

    size_t eocd = std::string::npos;
    for (size_t i = tail.size() - 22 + 1; i-- > 0;)
    {
        if (U32(&tail[i]) == EndOfCentralDirSig)
        {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos)
    {
        Log::Error("[RESOURCES] No end-of-central-directory record: " + path);
        return false;
    }

    uint32_t entryCount = U16(&tail[eocd + 10]);
    uint32_t dirSize = U32(&tail[eocd + 12]);
    uint32_t dirOffset = U32(&tail[eocd + 16]);
    if (entryCount == 0xFFFF || dirSize == 0xFFFFFFFF || dirOffset == 0xFFFFFFFF)
    {
        Log::Error("[RESOURCES] zip64 archives are not supported: " + path);
        return false;
    }
    if (static_cast<std::streamoff>(dirOffset) + dirSize > fileSize)
    {
        Log::Error("[RESOURCES] Corrupt central directory: " + path);
        return false;
    }

    std::vector<uint8_t> dir(dirSize);
    file.seekg(dirOffset);
    file.read(reinterpret_cast<char*>(dir.data()), dirSize);
    if (!file)
    {
        Log::Error("[RESOURCES] Failed reading central directory: " + path);
        return false;
    }

    std::vector<std::pair<std::string, Entry>> found;
    size_t pos = 0;
    size_t skipped = 0;
    while (pos + 46 <= dir.size() && U32(&dir[pos]) == CentralHeaderSig)
    {
        const uint8_t* h = &dir[pos];
        uint16_t flags = U16(h + 8);
        uint16_t nameLen = U16(h + 28);
        uint16_t extraLen = U16(h + 30);
        uint16_t commentLen = U16(h + 32);
        size_t next = pos + 46 + nameLen + extraLen + commentLen;
        if (next > dir.size()) break;

        std::string name(reinterpret_cast<const char*>(h + 46), nameLen);
        std::replace(name.begin(), name.end(), '\\', '/');
        pos = next;
        if (name.empty() || name.back() == '/') continue;

        Entry entry;
        entry.method = U16(h + 10);
        entry.crc = U32(h + 16);
        entry.compressedSize = U32(h + 20);
        entry.uncompressedSize = U32(h + 24);
        entry.localOffset = U32(h + 42);

        if ((flags & 1) != 0 || entry.compressedSize == 0xFFFFFFFF
            || entry.uncompressedSize == 0xFFFFFFFF || entry.localOffset == 0xFFFFFFFF
            || (entry.method != 0 && entry.method != 8))
        {
            Log::Error("[RESOURCES] Skipping unsupported (encrypted/zip64/method) entry: " + name);
            ++skipped;
            continue;
        }
        found.emplace_back(std::move(name), entry);
    }

    // A single top-level folder holding assets/ is a wrapper: strip it.
    bool hasRootAssets = false;
    std::string top;
    bool singleTop = !found.empty();
    for (const auto& [name, entry] : found)
    {
        if (name.rfind("assets/", 0) == 0) hasRootAssets = true;
        size_t slash = name.find('/');
        std::string first = slash == std::string::npos ? std::string() : name.substr(0, slash + 1);
        if (first.empty()) singleTop = false;
        else if (top.empty()) top = first;
        else if (top != first) singleTop = false;
    }
    if (!hasRootAssets && singleTop && std::any_of(found.begin(), found.end(),
            [&](const auto& item) { return item.first.rfind(top + "assets/", 0) == 0; }))
    {
        prefix = top;
        Log::Info("[RESOURCES] Stripping wrapper folder '" + prefix + "' from " + path);
    }

    for (auto& [name, entry] : found)
    {
        if (!prefix.empty())
        {
            if (name.rfind(prefix, 0) != 0) continue;
            name.erase(0, prefix.size());
        }
        if (index.emplace(name, entry).second) names.push_back(name);
    }

    Log::Info("[RESOURCES] Opened " + path + " (" + std::to_string(names.size()) + " files"
        + (skipped != 0 ? ", " + std::to_string(skipped) + " skipped" : std::string()) + ")");
    return true;
}

std::optional<std::vector<uint8_t>> ZipArchive::Read(const std::string& entryName)
{
    auto found = index.find(entryName);
    if (found == index.end()) return std::nullopt;
    const Entry& entry = found->second;

    std::vector<uint8_t> compressed(entry.compressedSize);
    {
        std::lock_guard<std::mutex> lock(fileMutex);
        uint8_t local[30];
        file.clear();
        file.seekg(entry.localOffset);
        file.read(reinterpret_cast<char*>(local), sizeof(local));
        if (!file || U32(local) != LocalHeaderSig)
        {
            Log::Error("[RESOURCES] Bad local header for " + entryName);
            return std::nullopt;
        }
        file.seekg(static_cast<std::streamoff>(entry.localOffset) + 30 + U16(local + 26) + U16(local + 28));
        file.read(reinterpret_cast<char*>(compressed.data()), entry.compressedSize);
        if (!file)
        {
            Log::Error("[RESOURCES] Failed reading data for " + entryName);
            return std::nullopt;
        }
    }

    std::vector<uint8_t> output;
    if (entry.method == 0)
    {
        output = std::move(compressed);
    }
    else
    {
        output.resize(entry.uncompressedSize);
        z_stream stream{};
        if (inflateInit2(&stream, -15) != Z_OK) return std::nullopt;
        stream.next_in = compressed.data();
        stream.avail_in = entry.compressedSize;
        stream.next_out = output.data();
        stream.avail_out = entry.uncompressedSize;
        int result = inflate(&stream, Z_FINISH);
        uLong produced = stream.total_out;
        inflateEnd(&stream);
        if ((result != Z_STREAM_END && !(result == Z_OK && entry.uncompressedSize == 0))
            || produced != entry.uncompressedSize)
        {
            Log::Error("[RESOURCES] Inflate failed for " + entryName);
            return std::nullopt;
        }
    }

    uLong crc = crc32(0L, Z_NULL, 0);
    if (!output.empty()) crc = crc32(crc, output.data(), static_cast<uInt>(output.size()));
    if (static_cast<uint32_t>(crc) != entry.crc || output.size() != entry.uncompressedSize)
    {
        Log::Error("[RESOURCES] CRC mismatch for " + entryName);
        return std::nullopt;
    }
    return output;
}

} // namespace Volcano
