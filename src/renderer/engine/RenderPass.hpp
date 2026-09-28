#pragma once
#ifndef VOLCANO_ENGINE_RENDER_PASS_H
#define VOLCANO_ENGINE_RENDER_PASS_H

#include <string>
#include "FrameContext.hpp"

namespace Volcano::Engine {

class ShaderLibrary;

// One step of the frame — terrain, entities, the GUI, a mod's own overlay,
// ... RenderEngine owns an ordered list of these and records every enabled
// one, in order, into the same render pass each frame (see
// RenderEngine::SubmitFrame). Subclass it and hand it to
// RenderEngine::AddPass()/InsertPassBefore() to add a pass of your own.
//
// Lifecycle, all driven by the engine on the render (main) thread:
//   Create()          — once the device exists; build pipelines/buffers here.
//   Record()          — every frame while enabled.
//   ReloadShaders()   — after the GPU is idle, when shaders are reloaded
//                       (Graphics.ShaderPack changed, or "ReloadShaders").
//   Destroy()         — before the device goes away. Must tolerate being
//                       called after a Create() that threw part way.
class RenderPass {
public:
    explicit RenderPass(std::string passName) : name(std::move(passName)) {}
    virtual ~RenderPass() = default;

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    const std::string& GetName() const { return name; }

    bool IsEnabled() const { return enabled; }
    void SetEnabled(bool isEnabled) { enabled = isEnabled; }

    virtual void Create(const ShaderLibrary& /*shaders*/) {}
    virtual void Record(FrameContext& frame) = 0;
    virtual void Destroy() {}

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
};

} // namespace Volcano::Engine

#endif
