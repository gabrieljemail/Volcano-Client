#pragma once
#ifndef VOLCANO_ENGINE_SHADER_LIBRARY_H
#define VOLCANO_ENGINE_SHADER_LIBRARY_H

#include <string>
#include <vector>

namespace Volcano::Engine {

// Resolves a shader name ("terrain.vert", "entity.frag", ...) to SPIR-V
// bytes. This is the external shader slot: when a shader pack is active
// (Graphics.ShaderPack in settings.json, a folder under
// resources/shaderpacks/), any "<name>.spv" it contains replaces the
// built-in one from resources/shaders/, and anything it doesn't contain
// falls back to the built-in. A pack can also provide shaders that have no
// built-in at all — the "overlay.vert"/"overlay.frag" pair ShaderPackPass
// draws — which simply don't run without one.
//
// Holds no graphics API objects: it only finds and validates bytes, so
// passes on any backend share it, and it's safe to call SetActivePack()
// between frames and then RenderEngine::RequestShaderReload() to apply it.
class ShaderLibrary {
public:
    static constexpr const char* BUILT_IN_DIRECTORY = "resources/shaders";
    static constexpr const char* PACK_ROOT = "resources/shaderpacks";

    // Selects PACK_ROOT/packName/ as the override directory; "" turns
    // overrides off. A pack folder that doesn't exist is logged and turns
    // overrides off too, rather than failing startup over a stale setting.
    // Returns whether a pack is active afterwards.
    bool SetActivePack(const std::string& packName);
    const std::string& GetActivePack() const { return activePack; }
    bool HasActivePack() const { return !activePack.empty(); }

    // Pack override first, then the built-in. An override that's missing is
    // silent; one that's present but isn't valid SPIR-V is logged and
    // skipped. Empty if neither location has a usable file.
    std::vector<char> Load(const std::string& shaderName) const;

    // The active pack's copy only, never the built-in — for pack-only slots
    // like the overlay. Empty when there's no pack or it lacks the file.
    std::vector<char> LoadFromPack(const std::string& shaderName) const;
    bool PackProvides(const std::string& shaderName) const;

    // Folder names directly under PACK_ROOT, sorted — for a settings UI.
    std::vector<std::string> ListPacks() const;

    // Magic number + word alignment only: enough to reject a GLSL source
    // file or a truncated download before the driver sees it, not a full
    // validation (the backend's module/pipeline creation does the rest).
    static bool IsValidSpirv(const std::vector<char>& code);

private:
    std::string activePack;

    std::string PackFilePath(const std::string& shaderName) const;
    static std::string BuiltInFilePath(const std::string& shaderName);
};

} // namespace Volcano::Engine

#endif
