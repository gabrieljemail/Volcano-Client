#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include "ShaderLibrary.hpp"
#include "../../Helpers.hpp"
#include "../../Logger.hpp"

namespace Volcano::Engine {

namespace {

constexpr uint32_t SPIRV_MAGIC = 0x07230203;

} // namespace

bool ShaderLibrary::SetActivePack(const std::string& packName)
{
    if (packName.empty())
    {
        activePack.clear();
        return false;
    }

    std::error_code error;
    std::filesystem::path packPath = std::filesystem::path(PACK_ROOT) / packName;
    if (!std::filesystem::is_directory(packPath, error))
    {
        Log::Error("[ERROR] Shader pack \"" + packName + "\" not found at " + packPath.string() + " — using built-in shaders.");
        activePack.clear();
        return false;
    }

    activePack = packName;
    Log::Info("[INFO] Shader pack \"" + packName + "\" active.");
    return true;
}

std::vector<char> ShaderLibrary::Load(const std::string& shaderName) const
{
    std::vector<char> code = LoadFromPack(shaderName);
    if (!code.empty()) return code;

    code = ReadFile(BuiltInFilePath(shaderName));
    if (!IsValidSpirv(code))
    {
        Log::Error("[ERROR] Built-in shader " + BuiltInFilePath(shaderName) + " is missing or not valid SPIR-V.");
        return {};
    }
    return code;
}

std::vector<char> ShaderLibrary::LoadFromPack(const std::string& shaderName) const
{
    if (!HasActivePack()) return {};

    std::string path = PackFilePath(shaderName);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return {};

    std::vector<char> code = ReadFile(path);
    if (!IsValidSpirv(code))
    {
        Log::Error("[ERROR] Shader pack file " + path + " is not valid SPIR-V (compile it with glslc/glslangValidator first) — ignoring it.");
        return {};
    }

    Log::Info("[INFO] Using shader pack override: " + path);
    return code;
}

bool ShaderLibrary::PackProvides(const std::string& shaderName) const
{
    if (!HasActivePack()) return false;
    std::error_code error;
    return std::filesystem::is_regular_file(PackFilePath(shaderName), error);
}

std::vector<std::string> ShaderLibrary::ListPacks() const
{
    std::vector<std::string> packs;
    std::error_code error;
    if (!std::filesystem::is_directory(PACK_ROOT, error)) return packs;

    for (const auto& entry : std::filesystem::directory_iterator(PACK_ROOT, error))
    {
        if (entry.is_directory(error)) packs.push_back(entry.path().filename().string());
    }
    std::sort(packs.begin(), packs.end());
    return packs;
}

bool ShaderLibrary::IsValidSpirv(const std::vector<char>& code)
{
    if (code.size() < sizeof(uint32_t) * 5 || code.size() % sizeof(uint32_t) != 0) return false;

    uint32_t magic = 0;
    std::memcpy(&magic, code.data(), sizeof(magic));
    return magic == SPIRV_MAGIC;
}

std::string ShaderLibrary::PackFilePath(const std::string& shaderName) const
{
    return (std::filesystem::path(PACK_ROOT) / activePack / (shaderName + ".spv")).string();
}

std::string ShaderLibrary::BuiltInFilePath(const std::string& shaderName)
{
    return (std::filesystem::path(BUILT_IN_DIRECTORY) / (shaderName + ".spv")).string();
}

} // namespace Volcano::Engine
