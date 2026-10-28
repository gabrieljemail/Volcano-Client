#include "ResourceManager.hpp"
#include "GlobalState.hpp"
#include "Logger.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace Volcano {

namespace fs = std::filesystem;

namespace {

const char* const BaseRoot = "resources/minecraft";

bool HasExtension(const std::string& name, const std::string& ext)
{
    return ext.empty() || (name.size() >= ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0);
}

} // namespace

ResourceManager::ResourceManager(GlobalState* state)
    : state(state)
{
    instance = this;
    Reload();
}

ResourceManager::~ResourceManager()
{
    if (instance == this) instance = nullptr;
}

ResourceManager& ResourceManager::Instance()
{
    if (instance == nullptr)
    {
        Log::Error("[RESOURCES] Instance() called before ResourceManager was constructed");
        std::abort();
    }
    return *instance;
}

bool ResourceManager::IsSafe(const std::string& logicalPath)
{
    return !logicalPath.empty() && logicalPath.find("..") == std::string::npos
        && logicalPath.find('\\') == std::string::npos && logicalPath[0] != '/' && logicalPath.find(':') == std::string::npos;
}

std::string ResourceManager::DiskPath(const std::string& logicalPath) const
{
    return std::string(BaseRoot) + "/" + logicalPath;
}

std::string ResourceManager::FindPack(const std::string& name)
{
    if (name.empty() || !IsSafe(name) || name.find('/') != std::string::npos) return {};

    std::string beside = state->config->ResolvePath("resourcepacks/" + name);
    if (fs::exists(beside)) return beside;

    std::string fallback = "resources/resourcepacks/" + name;
    if (fs::exists(fallback))
    {
        Log::Info("[RESOURCES] '" + name + "' not in resourcepacks/ next to the exe, using " + fallback);
        return fallback;
    }
    return {};
}

void ResourceManager::Reload()
{
    std::vector<std::string> packs = state->config->GetStringArray("Graphics.ResourcePacks");
    if (!state->config->Has("Graphics.ResourcePacks")) state->config->SetStringArray("Graphics.ResourcePacks", packs);

    std::unique_lock<std::shared_mutex> lock(mutex);
    layers.clear();

    for (const std::string& name : packs)
    {
        std::string path = FindPack(name);
        if (path.empty())
        {
            if (warned.insert(name).second) Log::Error("[RESOURCES] Warning: resource pack not found, skipping: " + name);
            continue;
        }

        Layer layer;
        if (fs::is_directory(path))
        {
            layer.root = path;
        }
        else
        {
            layer.zip = ZipArchive::Open(path);
            if (layer.zip == nullptr)
            {
                if (warned.insert(name).second) Log::Error("[RESOURCES] Warning: couldn't open resource pack, skipping: " + name);
                continue;
            }
        }
        warned.erase(name);
        layers.push_back(std::move(layer));
    }

    Layer base;
    base.root = BaseRoot;
    layers.push_back(std::move(base));
    Log::Info("[RESOURCES] " + std::to_string(layers.size()) + " layer(s) active (including base)");
}

std::optional<std::vector<uint8_t>> ResourceManager::ReadBytes(const std::string& logicalPath) const
{
    if (!IsSafe(logicalPath)) return std::nullopt;

    std::shared_lock<std::shared_mutex> lock(mutex);
    for (const Layer& layer : layers)
    {
        if (layer.zip != nullptr)
        {
            if (!layer.zip->Exists(logicalPath)) continue;
            // A present-but-corrupt entry falls through to lower layers.
            auto bytes = layer.zip->Read(logicalPath);
            if (bytes) return bytes;
            continue;
        }

        std::ifstream file(layer.root + "/" + logicalPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;
        std::streamoff size = file.tellg();
        if (size < 0) continue;
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        if (file) return bytes;
    }
    return std::nullopt;
}

bool ResourceManager::Exists(const std::string& logicalPath) const
{
    if (!IsSafe(logicalPath)) return false;

    std::shared_lock<std::shared_mutex> lock(mutex);
    for (const Layer& layer : layers)
    {
        if (layer.zip != nullptr)
        {
            if (layer.zip->Exists(logicalPath)) return true;
            continue;
        }
        std::error_code error;
        if (fs::is_regular_file(layer.root + "/" + logicalPath, error)) return true;
    }
    return false;
}

void ResourceManager::ListDirectory(const std::string& root, const std::string& dir, const std::string& ext,
    bool recursive, std::set<std::string>& out)
{
    std::error_code error;
    fs::path start = fs::path(root) / dir;
    if (!fs::is_directory(start, error)) return;

    auto visit = [&](const fs::directory_entry& item)
    {
        std::error_code itemError;
        if (!item.is_regular_file(itemError)) return;
        std::string relative = fs::relative(item.path(), fs::path(root), itemError).generic_string();
        if (!itemError && HasExtension(relative, ext)) out.insert(relative);
    };

    if (recursive)
    {
        for (const auto& item : fs::recursive_directory_iterator(start, error)) visit(item);
    }
    else
    {
        for (const auto& item : fs::directory_iterator(start, error)) visit(item);
    }
}

std::vector<std::string> ResourceManager::List(const std::string& dir, const std::string& ext, bool recursive) const
{
    std::set<std::string> result;
    std::string prefix = dir.empty() ? std::string() : dir + "/";

    std::shared_lock<std::shared_mutex> lock(mutex);
    for (const Layer& layer : layers)
    {
        if (layer.zip == nullptr)
        {
            ListDirectory(layer.root, dir, ext, recursive, result);
            continue;
        }

        for (const std::string& name : layer.zip->Entries())
        {
            if (name.compare(0, prefix.size(), prefix) != 0 || !HasExtension(name, ext)) continue;
            if (!recursive && name.find('/', prefix.size()) != std::string::npos) continue;
            result.insert(name);
        }
    }
    return std::vector<std::string>(result.begin(), result.end());
}

} // namespace Volcano
