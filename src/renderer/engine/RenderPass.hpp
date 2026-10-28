#pragma once
#ifndef VOLCANO_ENGINE_RENDER_PASS_H
#define VOLCANO_ENGINE_RENDER_PASS_H

#include <string>
#include "FrameContext.hpp"
#include "mods/VolcanoModAPI.h"

namespace Volcano::Engine {

class ShaderLibrary;

// One step of the frame - terrain, entities, the GUI, a mod's own overlay,
// ... RenderEngine owns an ordered list of these and records every enabled
// one, in order, into the same render pass each frame (see
// RenderEngine::SubmitFrame). Subclass it and hand it to
// RenderEngine::AddPass()/InsertPassBefore() to add a pass of your own.
//
// Lifecycle, all driven by the engine on the render (main) thread:
//   Create()          - once the device exists; build pipelines/buffers here.
//   Record()          - every frame while enabled.
//   ReloadShaders()   - after the GPU is idle, when shaders are reloaded
//                       (Graphics.ShaderPack changed, or "ReloadShaders").
//   Destroy()         - before the device goes away. Must tolerate being
//                       called after a Create() that threw part way.
class VC_API RenderPass {
public:
    explicit RenderPass(std::string passName) : name(std::move(passName)) {}
    virtual ~RenderPass() = default;

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    const std::string& GetName() const { return name; }

    bool IsEnabled() const { return enabled; }
    void SetEnabled(bool isEnabled) { enabled = isEnabled; }

    // Smoothed CPU time of this pass's Record(), in ms - only maintained
    // while the debug HUD is visible (see RenderEngine::RecordPasses).
    float GetRecordMs() const { return recordMs; }
    void SetRecordMs(float ms) { recordMs = ms; }

    // Which part of the frame this pass records in (see PassStage):
    //  PRE_MAIN - before the main render pass, with no render pass open: for
    //             work that needs its own render pass or compute, such as a
    //             shadow map.
    //  MAIN     - the default: inside the main render pass, in list order.
    //  OVERLAY  - after the main pass has finished, drawing over the finished
    //             frame (nothing is cleared) before it is presented. Build
    //             pipelines against the usual GetRenderPass(); the overlay
    //             render pass is compatible with it. Depth is kept, but
    //             anything written to it also feeds the Hi-Z pyramid.
    // Within a stage, passes run in list order.
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
