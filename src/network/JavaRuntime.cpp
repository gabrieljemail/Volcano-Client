#include "JavaRuntime.hpp"
#include <filesystem>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include "../Logger.hpp"
#include "../platform/OS.hpp"
#include "../resources/ZipArchive.hpp"

namespace Volcano::JavaRuntime {

namespace {

namespace fs = std::filesystem;
using Platform::OS;

std::string Sha256Hex(const std::vector<uint8_t>& data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    EVP_Digest(data.data(), data.size(), digest, &size, EVP_sha256(), nullptr);
    static const char* HEX = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < size; i++)
    {
        out += HEX[digest[i] >> 4];
        out += HEX[digest[i] & 15];
    }
    return out;
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(file);
}

} // namespace

bool IsManagedPath(const std::string& javaPath)
{
    return fs::path(javaPath).lexically_normal() == fs::path(EXECUTABLE).lexically_normal();
}

bool Installed()
{
    std::error_code ec;
    return fs::exists(EXECUTABLE, ec);
}

bool Download(const std::function<void(uint64_t, uint64_t)>& progress, std::string& error)
{
#ifndef _WIN32
    error = "Automatic Java download is only available on Windows. Install Java " + std::to_string(MAJOR_VERSION)
        + " and set the world's Java path.";
    return false;
#else
    // 1. Ask Adoptium for the current Java release's package and its checksum.
    std::vector<uint8_t> listing;
    const std::string query = "https://api.adoptium.net/v3/assets/latest/" + std::to_string(MAJOR_VERSION)
        + "/hotspot?architecture=x64&image_type=jre&os=windows&vendor=eclipse";
    if (!OS::HttpGet(query, listing, error)) return false;
    nlohmann::json assets = nlohmann::json::parse(listing.begin(), listing.end(), nullptr, false);
    if (assets.is_discarded() || !assets.is_array() || assets.empty())
    {
        error = "Adoptium has no Java " + std::to_string(MAJOR_VERSION) + " runtime listed.";
        return false;
    }
    const nlohmann::json& package = assets[0]["binary"]["package"];
    const std::string link = package.value("link", "");
    const std::string checksum = package.value("checksum", "");
    if (link.empty() || checksum.empty())
    {
        error = "Adoptium's listing is missing the download link.";
        return false;
    }

    // 2. The archive, checked against the published hash.
    std::vector<uint8_t> archive;
    if (!OS::HttpGet(link, archive, error, progress)) return false;
    if (Sha256Hex(archive) != checksum)
    {
        error = "The downloaded Java didn't match Adoptium's checksum, so it was discarded.";
        return false;
    }

    // 3. Unpack into a scratch folder (dropping the archive's single top-level folder), then swap it in, so a
    //    half-unpacked Java is never mistaken for an installed one.
    const fs::path scratchArchive = std::string(DIRECTORY) + ".zip";
    const fs::path scratch = std::string(DIRECTORY) + ".partial";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    if (!WriteFile(scratchArchive, archive))
    {
        error = "Couldn't write " + scratchArchive.string() + ".";
        return false;
    }
    archive.clear();
    archive.shrink_to_fit();

    {
        std::unique_ptr<ZipArchive> zip = ZipArchive::Open(scratchArchive.string());
        if (zip == nullptr)
        {
            error = "The downloaded Java couldn't be opened.";
            return false;
        }
        for (const std::string& name : zip->Entries())
        {
            size_t slash = name.find('/');
            if (slash == std::string::npos || name.find("..") != std::string::npos) continue;
            std::string relative = name.substr(slash + 1);
            if (relative.empty()) continue;

            auto bytes = zip->Read(name);
            if (!bytes || !WriteFile(scratch / relative, *bytes))
            {
                error = "Couldn't unpack " + name + ".";
                fs::remove_all(scratch, ec);
                return false;
            }
        }
    }
    fs::remove(scratchArchive, ec);

    if (!fs::exists(scratch / "bin" / "java.exe", ec))
    {
        error = "The downloaded Java has no bin/java.exe.";
        fs::remove_all(scratch, ec);
        return false;
    }
    fs::remove_all(DIRECTORY, ec);
    if (ec)
    {
        error = "Couldn't remove the old Java folder: " + ec.message();
        return false;
    }

    // Antivirus and the search indexer often hold the freshly written files for a few seconds, and Windows refuses to
    // rename a folder with open files inside, so retry; failing that, copy the files across instead.
    std::error_code renameError;
    for (int attempt = 0; attempt < 10; attempt++)
    {
        fs::rename(scratch, DIRECTORY, renameError);
        if (!renameError) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (renameError)
    {
        Log::Warn("[JAVA] Renaming the Java folder failed (" + renameError.message() + "); copying it instead.");
        std::error_code copyError;
        fs::remove_all(DIRECTORY, copyError);
        fs::copy(scratch, DIRECTORY, fs::copy_options::recursive, copyError);
        if (copyError)
        {
            error = "Couldn't move Java into place: " + copyError.message();
            fs::remove_all(DIRECTORY, ec); // Never leave a partial copy that looks installed.
            return false;
        }
        fs::remove_all(scratch, ec); // Best effort; a leftover scratch folder is harmless.
    }
    Log::Info("[JAVA] Installed Java " + std::to_string(MAJOR_VERSION) + " (" + package.value("name", "") + ").");
    return true;
#endif
}

} // namespace Volcano::JavaRuntime
