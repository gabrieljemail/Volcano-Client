#pragma once
#ifndef VOLCANO_ENGINE_RENDER_ENGINE_H
#define VOLCANO_ENGINE_RENDER_ENGINE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "FrameContext.hpp"
#include "RenderPass.hpp"
#include "SceneView.hpp"
#include "ShaderLibrary.hpp"

namespace Volcano {
struct GlobalState;
class TextureManager;
}

namespace Volcano::Engine {

namespace Vulkan { class VulkanRenderEngine; }

// The boundary between the rendering engine and the rest of the client.
// Everything outside src/renderer/engine/ (RenderThread's frame loop,
// VolcanoClient.cpp's startup/shutdown, mods) reaches the renderer through
// this interface - reached via GlobalState::renderEngine - instead of
// calling into VulkanInit's globals or recording commands itself. A
// backend (VulkanRenderEngine is the only one today) implements the pure
// virtuals; the pass list and shader reloading are shared here.
//
// Every method runs on the render thread, which is the main (GLFW-owning)
// thread - see RenderThread's own class comment. Only
// RequestShaderReload() is safe to call from anywhere else.
class RenderEngine {
public:
    explicit RenderEngine(GlobalState* globalState) : state(globalState) {}
    virtual ~RenderEngine() = default;

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;

    // ---- Lifecycle ----

    // Creates the window, device and swapchain, then registers and creates
    // the built-in passes. Needs state->config loaded (window size, VSync,
    // Graphics.ShaderPack, ...).
    virtual void Init() = 0;

    // Points the world/non-cubic passes' texture binding at the loaded
    // block/item texture array. Call once TextureManager::LoadResourcePack()
    // has finished.
    virtual void AttachTextures(const TextureManager& textures) = 0;

    // Starts the ImGui backend against this engine's render pass - the GUI
    // pass draws nothing until this has run.
    virtual void AttachGUI(const TextureManager* textures) = 0;

    // Waits for the GPU, then tears down the GUI and every pass. Call once
    // the frame loop has exited, before stopping threads that still own
    // GPU buffers (MeshingThread) and before Shutdown().
    virtual void StopRendering() = 0;

    // Destroys the device, swapchain and window. Last call on the engine.
    virtual void Shutdown() = 0;

    virtual const char* GetBackendName() const = 0;

    // The Vulkan backend's public handle/factory API (VulkanRenderEngine.hpp),
    // or nullptr on any other backend. Passes and mods that need Vulkan
    // objects go through this instead of reaching for VulkanInit's globals.
    virtual Vulkan::VulkanRenderEngine* GetVulkan() { return nullptr; }

    // ---- Frames ----

    // Handles any pending resize/shader reload, then waits for this frame
    // slot to free up and acquires an output image. False means skip this
    // frame (e.g. a stale or minimized swapchain) - don't call SubmitFrame().
    virtual bool BeginFrame() = 0;

    // Records every enabled pass for scene, submits, and presents.
    virtual void SubmitFrame(const SceneView& scene) = 0;

    // Current output size - read after BeginFrame(), which may resize it.
    virtual RenderExtent GetExtent() const = 0;
    virtual uint32_t GetFramesInFlight() const = 0;

    // Blocks until the GPU has finished everything submitted so far.
    virtual void WaitIdle() = 0;

    // ---- Window ----

    // F11: cycles windowed <-> borderless fullscreen.
    virtual void ToggleWindowMode() = 0;

    // Re-applies Window.Mode/Width/Height/FullscreenMode.* if the config
    // changed since last call (cheap otherwise) - called every frame so a
    // Settings screen change takes effect live.
    virtual void ApplyWindowSettings() = 0;

    // F2: saves the next presented frame (HUD included) to screenshots/.
    virtual void RequestScreenshot() = 0;

    // ---- Passes ----

    // Passes record by stage (RenderPass::GetStage): PRE_MAIN passes first,
    // then MAIN passes inside the main render pass sharing one color + depth
    // target, then OVERLAY passes over the finished frame; list order decides
    // within a stage. Built-in names, in order: "Shadows" (PRE_MAIN),
    // "Skybox", "Terrain", "Entities", "NonCubic", "Clouds", "Selection",
    // "ShaderPackOverlay", "GUI" (all MAIN). There are no built-in OVERLAY passes.
    //
    // Adding a pass after Init() creates it immediately; a pass whose
    // Create() throws is logged and not added (nullptr is returned). Names
    // must be unique - a duplicate is rejected the same way.
    RenderPass* AddPass(std::unique_ptr<RenderPass> pass);
    RenderPass* InsertPassBefore(const std::string& existingPass, std::unique_ptr<RenderPass> pass);
    RenderPass* InsertPassAfter(const std::string& existingPass, std::unique_ptr<RenderPass> pass);

    // Waits for the GPU, destroys the pass and drops it from the list.
    bool RemovePass(const std::string& passName);

    RenderPass* FindPass(const std::string& passName) const;
    std::vector<std::string> GetPassNames() const;

    // {name, smoothed Record() CPU ms} per pass, in draw order. Values only
    // update while GlobalState::debugHudVisible.
    std::vector<std::pair<std::string, float>> GetPassRecordTimes() const;

    // ---- Shaders ----

    ShaderLibrary& GetShaderLibrary() { return shaders; }

    // Switches Graphics.ShaderPack ("" = built-in shaders only), saves it,
    // and reloads every pass's shaders at the start of the next frame.
    void SetShaderPack(const std::string& packName);

    // WaitIdle(), then RenderPass::ReloadTextures() on every pass. Part of a
    // resource reload (see ResourceReloader); render thread, between frames.
    void ReloadPassTextures();

    // Rebuilds every pass's pipelines from the ShaderLibrary at the start
    // of the next frame - pick up edited .spv files without restarting.
    void RequestShaderReload() { shaderReloadRequested.store(true); }

protected:
    GlobalState* state;
    ShaderLibrary shaders;
    std::vector<std::unique_ptr<RenderPass>> passes;
    bool passesCreated = false;
    std::atomic<bool> shaderReloadRequested{false};

    // Create()/Destroy() every pass. CreatePasses() falls back to the
    // built-in shaders (dropping the active shader pack) and retries once
    // if a pass fails to build while a pack is active, so a broken pack
    // can't keep the client from starting.
    void CreatePasses();
    void DestroyPasses();

    // WaitIdle(), then ReloadShaders() on every pass - with the same
    // shader-pack fallback as CreatePasses(). Backends call this from
    // BeginFrame() once shaderReloadRequested is set.
    void ReloadPassShaders();

    // Record() every enabled pass of `stage`, in order. The first call of a
    // frame (PRE_MAIN) also resets the per-frame debug counters.
    void RecordPasses(FrameContext& frame, PassStage stage);

    // Whether any enabled pass records in `stage` - lets a backend skip
    // opening a render pass for an empty stage.
    bool HasEnabledPass(PassStage stage) const;

private:
    RenderPass* InsertAt(size_t index, std::unique_ptr<RenderPass> pass);
    size_t IndexOf(const std::string& passName) const;
};

} // namespace Volcano::Engine

#endif
