#pragma once
#ifndef VOLCANO_ENGINE_RENDER_PASS_H
#define VOLCANO_ENGINE_RENDER_PASS_H

#include <string>
#include "FrameContext.hpp"
#include "mods/VolcanoModAPI.h"

namespace Volcano::Engine {

class ShaderLibrary;

// One step of the frame
class VC_API RenderPass {
public:
    explicit RenderPass(std::string passName) : name(std::move(passName)) {}
    virtual ~RenderPass() = default;

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    const std::string& GetName() const { return name; }

    bool IsEnabled() const { return enabled; }
    void SetEnabled(bool isEnabled) { enabled = isEnabled; }

    // Smoothed CPU time of this pass's Record(), in ms
    float GetRecordMs() const { return recordMs; }
    void SetRecordMs(float ms) { recordMs = ms; }

    // Which part of the frame this pass records in (see PassStage):
    //  PRE_MAIN
    virtual PassStage GetStage() const { return PassStage::MAIN; }

    virtual void Create(const ShaderLibrary& /*shaders*/) {}
    virtual void Record(FrameContext& frame) = 0;
    virtual void Destroy() {}

    // After a resource reload (GPU idle): re-read any pack textures the pass
    // owns. Default does nothing; passes with their own pack art override it.
    virtual void ReloadTextures(const ShaderLibrary& /*shaders*/) {}

    // Default is a full rebuild; passes whose Create() also uploads
    // meshes/textures override this to rebuild only their pipelines.
    virtual void ReloadShaders(const ShaderLibrary& shaders)
    {
        Destroy();
        Create(shaders);
    }

private:
    std::string name;
    bool enabled = true;
    float recordMs = 0.0f;
};

} // namespace Volcano::Engine

#endif
