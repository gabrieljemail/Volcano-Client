#define NOMINMAX
#include <Windows.h>
#include <d3dcompiler.h>
#include <set>
#include <wrl/client.h>
#include <fstream>
#include <stdexcept>
#include <string>

#include "D3D11Shader.hpp"

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace Volcano::Engine::D3D11 {

namespace {

struct ShaderTarget {
    const char* entryPoint;
    const char* target;
};

ShaderTarget TargetFor(const std::string& name)
{
    // spirv-cross emits a `main(SPIRV_Cross_Input)` wrapper around each
    // stage's *_main body - "main" is the entry point for every stage.
    // SM 4.0 is the floor every FL 11_0 part supports (see CMakeLists); the
    // compute shaders need 5.0 for RWTexture* UAVs, which every FL 11_0 part
    // also runs.
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".vert") == 0) return { "main", "vs_4_0" };
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".frag") == 0) return { "main", "ps_4_0" };
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".comp") == 0) return { "main", "cs_5_0" };
    throw std::runtime_error("[ERROR] Unknown shader stage in \"" + name + "\" (expected .vert/.frag/.comp).");
}

// Vulkan numbers resources per descriptor set, so a GLSL shader can have a
// texture at set 0 binding 0 and another at set 1 binding 0; spirv-cross
// flattens both to register(t0)/register(s0), and D3DCompile refuses
// overlapping registers (X4500). Moves each repeat of a t#/s#/b#/u# register to
// the lowest free index of its class, in declaration order, so the first
// declaration keeps its slot and the next ones take t1, t2, ... - the slots the
// passes bind them to.
std::string ResolveRegisterCollisions(const std::string& source)
{
    struct Register
    {
        size_t digitsBegin = 0; // Where the number starts in `source`.
        size_t digitsEnd = 0;
        char kind = 0;
        int index = 0;
    };

    std::vector<Register> registers;
    for (size_t at = source.find("register("); at != std::string::npos; at = source.find("register(", at + 1))
    {
        size_t kindAt = at + 9;
        if (kindAt + 1 >= source.size()) break;
        char kind = source[kindAt];
        if (kind != 't' && kind != 's' && kind != 'b' && kind != 'u') continue;
        size_t digitsBegin = kindAt + 1;
        size_t digitsEnd = digitsBegin;
        while (digitsEnd < source.size() && source[digitsEnd] >= '0' && source[digitsEnd] <= '9') digitsEnd++;
        if (digitsEnd == digitsBegin) continue;
        registers.push_back({ digitsBegin, digitsEnd, kind, std::stoi(source.substr(digitsBegin, digitsEnd - digitsBegin)) });
    }

    std::set<std::pair<char, int>> taken;
    std::vector<size_t> duplicates;
    for (size_t i = 0; i < registers.size(); i++)
    {
        if (!taken.insert({ registers[i].kind, registers[i].index }).second) duplicates.push_back(i);
    }
    if (duplicates.empty()) return source;

    // Rewrite back to front so earlier offsets stay valid.
    std::vector<int> assigned(registers.size(), -1);
    for (size_t i : duplicates)
    {
        int index = 0;
        while (taken.count({ registers[i].kind, index }) != 0) index++;
        taken.insert({ registers[i].kind, index });
        assigned[i] = index;
    }
    std::string result = source;
    for (size_t i = registers.size(); i-- > 0;)
    {
        if (assigned[i] < 0) continue;
        result.replace(registers[i].digitsBegin, registers[i].digitsEnd - registers[i].digitsBegin,
            std::to_string(assigned[i]));
    }
    return result;
}

} // namespace

// Helpers.hpp's typedefs collide with the Windows headers this TU needs, so
// the file read lives here instead of coming from there.
std::vector<char> ReadShaderFile(const std::string& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) return {};
    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

std::vector<char> CompileShader(const std::string& name)
{
    std::string path = "resources/shaders/" + name + ".hlsl";
    std::vector<char> file = ReadShaderFile(path);
    if (file.empty()) throw std::runtime_error("[ERROR] Shader file not found: " + path);
    std::string resolved = ResolveRegisterCollisions(std::string(file.begin(), file.end()));
    std::vector<char> source(resolved.begin(), resolved.end());
    source.push_back('\0');

    ShaderTarget target = TargetFor(name);
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    HRESULT result = D3DCompile(source.data(), source.size() - 1, path.c_str(), nullptr, nullptr,
        target.entryPoint, target.target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(result))
    {
        std::string message = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "";
        throw std::runtime_error("[ERROR] HLSL compile failed for " + path + ": " + message);
    }
    if (code == nullptr || code->GetBufferSize() == 0)
    {
        throw std::runtime_error("[ERROR] HLSL compile produced no bytecode for " + path);
    }

    const char* bytes = static_cast<const char*>(code->GetBufferPointer());
    return std::vector<char>(bytes, bytes + code->GetBufferSize());
}

namespace {

template<typename T>
ComPtr<T> CreateShader(ID3D11Device* device, const std::string& name,
    HRESULT (ID3D11Device::*create)(const void*, SIZE_T, ID3D11ClassLinkage*, T**))
{
    std::vector<char> code = CompileShader(name);
    ComPtr<T> shader;
    HRESULT result = (device->*create)(code.data(), code.size(), nullptr, &shader);
    if (FAILED(result) || shader == nullptr)
    {
        throw std::runtime_error("[ERROR] ID3D11Device shader creation failed for resources/shaders/" + name + ".hlsl.");
    }
    return shader;
}

} // namespace

Microsoft::WRL::ComPtr<ID3D11VertexShader> CreateVertexShader(ID3D11Device* device, const std::string& name)
{
    return CreateShader<ID3D11VertexShader>(device, name, &ID3D11Device::CreateVertexShader);
}

Microsoft::WRL::ComPtr<ID3D11PixelShader> CreatePixelShader(ID3D11Device* device, const std::string& name)
{
    return CreateShader<ID3D11PixelShader>(device, name, &ID3D11Device::CreatePixelShader);
}

Microsoft::WRL::ComPtr<ID3D11ComputeShader> CreateComputeShader(ID3D11Device* device, const std::string& name)
{
    return CreateShader<ID3D11ComputeShader>(device, name, &ID3D11Device::CreateComputeShader);
}

} // namespace Volcano::Engine::D3D11
