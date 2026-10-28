#pragma once
#ifndef VOLCANO_RESOURCES_ZIP_ARCHIVE_H
#define VOLCANO_RESOURCES_ZIP_ARCHIVE_H

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Volcano {

// Read-only .zip reader. Indexes the central directory once on Open() and
// keeps the file open; stored and deflate entries only (encrypted and zip64
// entries are logged and skipped). Read() is thread-safe (internal mutex).
class ZipArchive {
public:
    // Returns null (and logs) if path isn't a readable zip.
    static std::unique_ptr<ZipArchive> Open(const std::string& path);

    // Inflated contents of entry, or nullopt if missing, unsupported or the
    // CRC32 doesn't match. Entry names use forward slashes, case-sensitive.
    std::optional<std::vector<uint8_t>> Read(const std::string& entry);

    bool Exists(const std::string& entry) const { return index.count(entry) != 0; }

    // Every readable file entry (directories excluded), with any stripped
    // wrapper folder prefix already removed.
    const std::vector<std::string>& Entries() const { return names; }

    // Wrapper folder that was stripped (with trailing slash), or empty.
    const std::string& StrippedPrefix() const { return prefix; }

private:
    struct Entry
    {
        uint32_t localOffset{0};
        uint32_t compressedSize{0};
        uint32_t uncompressedSize{0};
        uint32_t crc{0};
        uint16_t method{0};
    };

    ZipArchive() = default;
    bool Index(const std::string& path);

    std::ifstream file;
    std::mutex fileMutex;
    std::unordered_map<std::string, Entry> index;
    std::vector<std::string> names;
    std::string prefix;
};

} // namespace Volcano

#endif
