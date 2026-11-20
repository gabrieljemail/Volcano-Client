#pragma once
#ifndef VOLCANO_ENGINE_D3D11_SHADER_H
#define VOLCANO_ENGINE_D3D11_SHADER_H

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace Volcano::Engine::D3D11 {

// DXBC bytecode for resources/shaders/<name>.hlsl - the spirv-cross twin of
// the .spv file ShaderLibrary serves the Vulkan backend (the CMake Shaders
// target generates both from the same GLSL). `name` includes the stage,
// e.g. "terrain.vert". Shader packs are SPIR-V-only for now, so this always
// resolves the built-in copy. Throws std::runtime_error on a missing file,
// a failed compile, or an unknown stage.
std::vector<char> CompileShader(const std::string& name);

// Ready-made shader objects over CompileShader's bytecode. Entry points are
// the cross-compiler's vert_main/frag_main/comp_main.
Microsoft::WRL::ComPtr<ID3D11VertexShader> CreateVertexShader(ID3D11Device* device, const std::string& name);
Microsoft::WRL::ComPtr<ID3D11PixelShader> CreatePixelShader(ID3D11Device* device, const std::string& name);
Microsoft::WRL::ComPtr<ID3D11ComputeShader> CreateComputeShader(ID3D11Device* device, const std::string& name);

} // namespace Volcano::Engine::D3D11

#endif
