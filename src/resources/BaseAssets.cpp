#include "BaseAssets.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include "ZipArchive.hpp"
#include "../Logger.hpp"
#include "../platform/OS.hpp"

namespace Volcano::BaseAssets {

namespace {

namespace fs = std::filesystem;
using Platform::OS;

constexpr const char* MANIFEST_URL = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
constexpr const char* OBJECTS_URL = "https://resources.download.minecraft.net/";
constexpr const char* SOUND_PREFIX = "minecraft/sounds/";
constexpr const char* ASSETS_ROOT = "assets/minecraft/";
constexpr size_t SOUND_WORKERS = 6;

// Share of the bar each step gets.
constexpr float JAR_END = 0.35f;
constexpr float EXTRACT_END = 0.45f;

std::string Sha1Hex(const std::vector<uint8_t>& data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    EVP_Digest(data.data(), data.size(), digest, &size, EVP_sha1(), nullptr);
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
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(file);
}

// Fetches a JSON document.
bool GetJson(const std::string& url, nlohmann::json& out, std::string& error)
{
    std::vector<uint8_t> bytes;
    if (!OS::HttpGet(url, bytes, error)) return false;
    out = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (out.is_discarded())
    {
        error = "The server sent something that isn't JSON: " + url;
        return false;
    }
    return true;
}

} // namespace

bool Installed()
{
    std::error_code error;
    return fs::exists(fs::path(DIRECTORY) / "version.json", error)
        && fs::is_directory(fs::path(DIRECTORY) / ASSETS_ROOT / "sounds", error);
}

bool Download(const ProgressFn& progress, std::string& error)
{
    auto report = [&](float fraction, const std::string& status)
    {
        if (progress) progress(std::clamp(fraction, 0.0f, 1.0f), status);
    };

    // 1. The version list, then this version's file list.
    report(0.0f, "Finding version " + std::string(VERSION) + "...");
    nlohmann::json manifest;
    if (!GetJson(MANIFEST_URL, manifest, error)) return false;
    std::string versionUrl;
    for (const nlohmann::json& entry : manifest.value("versions", nlohmann::json::array()))
    {
        if (entry.value("id", "") == VERSION) versionUrl = entry.value("url", "");
    }
    if (versionUrl.empty())
    {
        error = "Mojang's version list has no " + std::string(VERSION) + ".";
        return false;
    }

    nlohmann::json version;
    if (!GetJson(versionUrl, version, error)) return false;
    const nlohmann::json& client = version["downloads"]["client"];
    std::string jarUrl = client.value("url", "");
    std::string jarSha1 = client.value("sha1", "");
    std::string indexUrl = version["assetIndex"].value("url", "");
    if (jarUrl.empty() || jarSha1.empty() || indexUrl.empty())
    {
        error = "The version file is missing its download links.";
        return false;
    }

    // 2. The client jar, checked against the hash Mojang published.
    std::vector<uint8_t> jar;
    auto lastReport = std::chrono::steady_clock::now();
    bool fetched = OS::HttpGet(jarUrl, jar, error, [&](uint64_t received, uint64_t total)
    {
        auto now = std::chrono::steady_clock::now();
        if (now - lastReport < std::chrono::milliseconds(50)) return;
        lastReport = now;
        float part = total > 0 ? static_cast<float>(received) / static_cast<float>(total) : 0.0f;
        report(part * JAR_END, "Downloading game files (" + std::to_string(received / (1024 * 1024)) + " MB)...");
    });
    if (!fetched) return false;
    if (Sha1Hex(jar) != jarSha1)
    {
        error = "The downloaded game files didn't match Mojang's checksum, so they were discarded.";
        return false;
    }

    // 3. Unpack everything except code. version.json goes last: it marks the folder as usable.
    report(JAR_END, "Unpacking...");
    const fs::path temporary = fs::path(DIRECTORY).string() + ".jar";
    if (!WriteFile(temporary, jar))
    {
        error = "Couldn't write to " + temporary.string() + ".";
        return false;
    }
    jar.clear();
    jar.shrink_to_fit();

    std::vector<uint8_t> versionFile;
    {
        std::unique_ptr<ZipArchive> zip = ZipArchive::Open(temporary.string());
        if (zip == nullptr)
        {
            error = "The downloaded game files couldn't be opened.";
            return false;
        }
        const std::vector<std::string>& names = zip->Entries();
        for (size_t i = 0; i < names.size(); i++)
        {
            const std::string& name = names[i];
            bool code = name.size() > 6 && name.compare(name.size() - 6, 6, ".class") == 0;
            if (code || name.rfind("META-INF/", 0) == 0) continue;
            if (name.find("..") != std::string::npos) continue; // Never write outside the folder.

            auto bytes = zip->Read(name);
            if (!bytes) continue;
            if (name == "version.json")
            {
                versionFile = std::move(*bytes);
                continue;
            }
            if (!WriteFile(fs::path(DIRECTORY) / name, *bytes))
            {
                error = "Couldn't write " + name + ".";
                return false;
            }
            if (i % 200 == 0)
            {
                report(JAR_END + (EXTRACT_END - JAR_END) * static_cast<float>(i) / static_cast<float>(names.size()),
                    "Unpacking...");
            }
        }
    }
    std::error_code ignored;
    fs::remove(temporary, ignored);

    // 4. The sounds, which live outside the jar as hashed files. Existing files of the right size are kept.
    report(EXTRACT_END, "Finding the sound files...");
    nlohmann::json index;
    if (!GetJson(indexUrl, index, error)) return false;

    struct Sound
    {
        std::string path;
        std::string hash;
        uint64_t size;
    };
    std::vector<Sound> sounds;
    for (const auto& [key, entry] : index.value("objects", nlohmann::json::object()).items())
    {
        if (key.rfind(SOUND_PREFIX, 0) != 0 || key.find("..") != std::string::npos) continue;
        std::string hash = entry.value("hash", "");
        if (hash.size() < 3) continue;
        sounds.push_back({ "assets/" + key, hash, entry.value("size", 0ull) }); // minecraft/sounds/x -> assets/minecraft/sounds/x
    }

    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
    std::atomic<size_t> failed{0};
    std::mutex errorMutex;
    std::string firstError;

    auto work = [&]
    {
        for (;;)
        {
            size_t i = next.fetch_add(1);
            if (i >= sounds.size()) return;
            const Sound& sound = sounds[i];
            fs::path target = fs::path(DIRECTORY) / sound.path;
            std::error_code ec;
            if (fs::exists(target, ec) && fs::file_size(target, ec) == sound.size)
            {
                done++;
                continue;
            }

            std::vector<uint8_t> bytes;
            std::string why;
            bool ok = false;
            for (int attempt = 0; attempt < 3 && !ok; attempt++)
            {
                ok = OS::HttpGet(std::string(OBJECTS_URL) + sound.hash.substr(0, 2) + "/" + sound.hash, bytes, why)
                    && Sha1Hex(bytes) == sound.hash && WriteFile(target, bytes);
            }
            if (!ok)
            {
                failed++;
                std::lock_guard<std::mutex> lock(errorMutex);
                if (firstError.empty()) firstError = sound.path + (why.empty() ? "" : ": " + why);
            }
            done++;
        }
    };

    std::vector<std::thread> workers;
    for (size_t i = 0; i < SOUND_WORKERS; i++) workers.emplace_back(work);
    while (done.load() < sounds.size())
    {
        float part = sounds.empty() ? 1.0f : static_cast<float>(done.load()) / static_cast<float>(sounds.size());
        report(EXTRACT_END + (1.0f - EXTRACT_END) * part,
            "Downloading sounds (" + std::to_string(done.load()) + " of " + std::to_string(sounds.size()) + ")...");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (std::thread& worker : workers) worker.join();

    if (failed.load() > 0)
    {
        error = std::to_string(failed.load()) + " sound file(s) failed to download (" + firstError + "). Run again to retry.";
        return false;
    }

    if (!WriteFile(fs::path(DIRECTORY) / "version.json", versionFile))
    {
        error = "Couldn't write version.json.";
        return false;
    }
    Log::Info("[RESOURCES] Downloaded the base assets for " + std::string(VERSION) + ".");
    report(1.0f, "Done.");
    return true;
}

} // namespace Volcano::BaseAssets
