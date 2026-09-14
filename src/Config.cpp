#include "Config.hpp"
#include "Logger.hpp"
#include <fstream>
#include <sstream>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace Volcano {

namespace {

// Directory containing the running executable, so settings.json lives next
// to it regardless of the process's current working directory.
std::string ExecutableDirectory()
{
    std::string exePath;

#ifdef _WIN32
    char buffer[MAX_PATH];
    DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length > 0) exePath.assign(buffer, length);
#else
    char buffer[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer));
    if (length > 0) exePath.assign(buffer, static_cast<size_t>(length));
#endif

    if (exePath.empty()) return ".";
    size_t slash = exePath.find_last_of("/\\");
    return slash == std::string::npos ? "." : exePath.substr(0, slash);
}

std::vector<std::string> SplitKey(const std::string& key)
{
    std::vector<std::string> segments;
    std::stringstream stream(key);
    std::string segment;
    while (std::getline(stream, segment, '.')) segments.push_back(segment);
    return segments;
}

nlohmann::json ToJson(const ConfigValue& value)
{
    return std::visit([](const auto& v) { return nlohmann::json(v); }, value);
}

// Tries to read node as whichever ConfigValue alternative fallback holds,
// writing the result into out. Returns false if node holds some other JSON
// type (or a value that doesn't fit, e.g. a negative number into uint32_t)
// — the key is then treated as unset.
bool ExtractMatching(const nlohmann::json& node, const ConfigValue& fallback, ConfigValue& out)
{
    return std::visit([&](const auto& fallbackValue) {
        using T = std::decay_t<decltype(fallbackValue)>;
        try {
            out = node.get<T>();
            return true;
        } catch (const nlohmann::json::exception&) {
            return false;
        }
    }, fallback);
}

} // namespace

const nlohmann::json* Config::Find(const nlohmann::json& root, const std::string& key)
{
    const nlohmann::json* node = &root;
    for (const std::string& segment : SplitKey(key))
    {
        if (!node->is_object() || !node->contains(segment)) return nullptr;
        node = &(*node)[segment];
    }
    return node;
}

void Config::Assign(nlohmann::json& root, const std::string& key, nlohmann::json value)
{
    std::vector<std::string> segments = SplitKey(key);

    nlohmann::json* node = &root;
    for (size_t i = 0; i + 1 < segments.size(); ++i)
    {
        node = &(*node)[segments[i]];
    }
    (*node)[segments.back()] = std::move(value);
}

void Config::Load()
{
    path = ExecutableDirectory() + "/settings.json";

    std::ifstream file(path);
    if (file.is_open())
    {
        try {
            file >> data;
        } catch (const nlohmann::json::exception& e) {
            Log::Error(std::string("[CONFIG] Failed to parse settings.json, falling back to defaults: ") + e.what());
            data = nlohmann::json::object();
        }
    }
    if (!data.is_object()) data = nlohmann::json::object();

    activeInstance = this;
}

void Config::Save() const
{
    std::ofstream file(path);
    if (!file.is_open())
    {
        Log::Error("[CONFIG] Failed to write settings to " + path);
        return;
    }
    file << data.dump(4);
}

ConfigValue Config::Get(const std::string& key, const ConfigValue& fallback)
{
    const nlohmann::json* node = Find(data, key);
    ConfigValue value;
    if (node != nullptr && ExtractMatching(*node, fallback, value)) return value;

    // Unset or invalid — fall back to the caller's default, and persist it
    // so settings.json (and every later Get() for this key) reflects it.
    Assign(data, key, ToJson(fallback));
    Save();
    return fallback;
}

void Config::Set(const std::string& key, const ConfigValue& value)
{
    Assign(data, key, ToJson(value));
    Save();
}

} // namespace Volcano
