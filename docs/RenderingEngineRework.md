# Rendering Engine Rework

This rework makes three changes to the renderer:

1. **An abstraction layer** (`src/renderer/engine/`) sits between the
   rendering engine and the rest of the client. Nothing outside the
   renderer calls Vulkan or reads VulkanInit's globals anymore. Everything
   goes through `Engine::RenderEngine`, which you reach through
   `GlobalState::renderEngine`.
2. **The old `RenderThread::RecordAndSubmitFrame()` is split into render
   passes.** It used to be a ~200-line function that recorded every draw.
   Each draw is now a `RenderPass` object with its own `Create` / `Record` /
   `Destroy` lifecycle, and the engine runs them in order.
3. **An optional slot for external shaders** loads SPIR-V from a shader pack
   folder at runtime. A pack can replace any built-in shader or add a
   fullscreen overlay pass. You can reload it live with **F9**.

The frame the client draws is meant to be the same as before. Same passes,
same order, same pipelines, same sync. What changed is where the code lives
and who owns it.

---

## 1. Architecture

### Before

```
VolcanoClient.cpp ──► VulkanInit (globals: device, swapchain, pipelines, ...)
        │                   ▲
        ▼                   │  vkCmd*, vkQueueSubmit, fences, semaphores,
RenderThread ───────────────┘  camera UBO memcpy, descriptor binds, every draw
  (input, timing, FOV, sky color, acquire/record/submit/present, all passes)
```

`RenderThread` handled game-side work (input, frame pacing, FOV easing, sky
color) and GPU-side work (fences, command recording, submit, present) in one
place. `VolcanoClient.cpp` also wrote Vulkan descriptors and passed a
`VkRenderPass` to the GUI.

### After

```
                     ┌──────────────── client side ─────────────────┐
VolcanoClient.cpp ──►│ RenderThread: timing, input, FOV, sky color  │
                     │   builds Engine::SceneView (glm/POD only)    │
                     └───────────────┬──────────────────────────────┘
                                     │ GlobalState::renderEngine
                                     ▼
                     ┌──────── Engine::RenderEngine (abstract) ─────┐
                     │ BeginFrame / SubmitFrame / pass list /       │
                     │ ShaderLibrary / shader reload + fallback     │
                     └───────────────┬──────────────────────────────┘
                                     ▼
                     ┌──── Engine::Vulkan::VulkanRenderEngine ──────┐
                     │ acquire → camera UBO → begin pass → passes → │
                     │ end → submit → present      (wraps VulkanInit)│
                     └───────────────┬──────────────────────────────┘
                                     ▼  FrameContext
     Terrain → Entities → NonCubic → Selection → ShaderPackOverlay → GUI
```

### New files

| File | Role |
|---|---|
| `src/renderer/engine/RenderEngine.hpp/.cpp` | The abstract engine interface. Holds the ordered pass list, the `ShaderLibrary`, and the shader-reload and shader-pack-fallback logic shared by every backend. |
| `src/renderer/engine/SceneView.hpp` | What the client decides about a frame: view/proj, eye position, sky color, day brightness, time, delta time, lighting and wireframe flags. glm and plain types only. |
| `src/renderer/engine/FrameContext.hpp` | What a pass gets in `Record()`: engine, state, scene, frame-in-flight index, extent, command buffer, precomputed `viewProj`. Forward-declares `VkCommandBuffer`, so it doesn't pull in the Vulkan headers. |
| `src/renderer/engine/RenderPass.hpp` | Base class for a pass: name, enabled flag, and the `Create` / `Record` / `ReloadShaders` / `Destroy` lifecycle. |
| `src/renderer/engine/ShaderLibrary.hpp/.cpp` | Maps shader names to SPIR-V bytes. Looks in the shader pack first, then falls back to the built-in shader. Checks for the SPIR-V magic number. No graphics API types. |
| `src/renderer/engine/vulkan/VulkanRenderEngine.hpp/.cpp` | The Vulkan backend. Owns frame sync and submission, which used to live in `RenderThread`. |
| `src/renderer/engine/passes/ChunkMeshPass.hpp` | Shared base for the two chunk-mesh passes: binds the world descriptor sets and draws one `Mesh`. |
| `src/renderer/engine/passes/TerrainPass.hpp/.cpp` | Opaque chunks. Owns the terrain and wireframe pipelines and `pipelineLayout`. |
| `src/renderer/engine/passes/NonCubicPass.hpp/.cpp` | Transparent, partial-shape and cross-shaped blocks, sorted back to front. |
| `src/renderer/engine/passes/EntityPass.hpp` | Adapter around `EntityRenderer`. |
| `src/renderer/engine/passes/SelectionPass.hpp` | Adapter around `SelectionRenderer`. |
| `src/renderer/engine/passes/ShaderPackPass.hpp/.cpp` | The external-shader overlay slot. |
| `src/renderer/engine/passes/GUIPass.hpp` | ImGui draw data. |
| `resources/shaderpacks/Example/` | Example pack: an overlay vignette, as GLSL source plus compiled `.spv`. |

Namespaces follow the folders, as `CODESTYLE.mdx` asks:
`Volcano::Engine`, `Volcano::Engine::Passes`, `Volcano::Engine::Vulkan`.

---

## 2. The abstraction layer: `Engine::RenderEngine`

```cpp
class RenderEngine {
    // Lifecycle
    virtual void Init() = 0;                                   // window, device, swapchain, built-in passes
    virtual void AttachTextures(const TextureManager&) = 0;    // bind the block/item texture array
    virtual void AttachGUI(const TextureManager*) = 0;         // start ImGui against the engine's render pass
    virtual void StopRendering() = 0;                          // wait for GPU, tear down GUI and passes
    virtual void Shutdown() = 0;                               // destroy device, swapchain, window

    // Frames
    virtual bool BeginFrame() = 0;                             // resize, shader reload, fence wait, acquire
    virtual void SubmitFrame(const SceneView&) = 0;            // record passes, submit, present
    virtual RenderExtent GetExtent() const = 0;
    virtual uint32_t GetFramesInFlight() const = 0;
    virtual void WaitIdle() = 0;
    virtual void ToggleWindowMode() = 0;                       // F11

    // Passes (shared implementation)
    RenderPass* AddPass(std::unique_ptr<RenderPass>);
    RenderPass* InsertPassBefore(const std::string& existing, std::unique_ptr<RenderPass>);
    RenderPass* InsertPassAfter(const std::string& existing, std::unique_ptr<RenderPass>);
    bool RemovePass(const std::string& name);
    RenderPass* FindPass(const std::string& name) const;
    std::vector<std::string> GetPassNames() const;

    // Shaders (shared implementation)
    ShaderLibrary& GetShaderLibrary();
    void SetShaderPack(const std::string& packName);           // saves Graphics.ShaderPack, reloads next frame
    void RequestShaderReload();                                // thread-safe; applied at the next BeginFrame
};
```

Design notes:

- **`SceneView` is the only thing the client passes in for a frame.**
  `RenderThread` doesn't know about command buffers, UBOs, clear values,
  fences or image indices. The engine turns the `SceneView` into a camera
  UBO upload and the main pass's clear color.
- **The frame is split into `BeginFrame()` and `SubmitFrame()`.**
  `BeginFrame()` can recreate the swapchain after a resize, which changes
  the aspect ratio. So the client builds the projection *after*
  `BeginFrame()` returns, using `GetExtent()`. The old code had the same
  ordering implicitly (resize first, then `RecordAndSubmitFrame`).
- **`GlobalState::renderEngine`** follows the `CODESTYLE.mdx` rule for
  manager classes. Other systems and mod DLLs reach the engine the same way
  they reach `TickLoop` or `InteractionManager`.
- **Threading.** Everything runs on the render thread, which is the main
  GLFW thread (see `ThreadArchitecture.mdx`). The one exception is
  `RequestShaderReload()`, an atomic flag that is safe to set from any
  thread.
- **Backends.** `VulkanRenderEngine` is the only backend today. A future
  backend would implement the pure virtuals and provide its own passes. The
  pass list, `ShaderLibrary` and `SceneView` don't need to change. Passes
  themselves are backend-specific: a Vulkan pass records into
  `FrameContext::commandBuffer`.

### `RenderPass` lifecycle

| Hook | When | Default |
|---|---|---|
| `Create(shaders)` | Once the device exists: during `Init()`, or right away if the pass is added later. | no-op |
| `Record(frame)` | Every frame while `IsEnabled()`, inside the main render pass, with viewport, scissor and camera UBO already set. | pure virtual |
| `ReloadShaders(shaders)` | After `WaitIdle()`, on F9 or `SetShaderPack()`. | `Destroy(); Create(shaders);` |
| `Destroy()` | From `StopRendering()` in reverse order, from `RemovePass()`, and during shader-pack fallback. Must be idempotent and must handle a partial `Create()`. | no-op |

---

## 3. Splitting `RenderThread.cpp` into render passes

`RenderThread::RecordAndSubmitFrame()` plus `AcquireImage()` and
`PresentFrame()` were split up as follows:

| Old code in `RenderThread.cpp` | New home |
|---|---|
| Resize check at the top of `DrawFrame` | `VulkanRenderEngine::BeginFrame` |
| `AcquireImage()`: fence wait, acquire, reset | `VulkanRenderEngine::BeginFrame` |
| Begin command buffer | `VulkanRenderEngine::BeginCommandBuffer` |
| Sky color / `DayBrightness` | `RenderThread::BuildSceneView` (client side) → `SceneView::skyColor` |
| FOV easing and projection | `RenderThread::BuildProjection` (client side) |
| `CameraUBO` memcpy | `VulkanRenderEngine::UploadCamera` |
| Begin render pass with clear values, viewport, scissor | `VulkanRenderEngine::BeginMainPass` |
| Terrain pipeline bind (incl. F3 wireframe), descriptor binds, frustum-culled `renderList` draw | `Passes::TerrainPass::Record` |
| `EntityRenderer::RecordDraw(...)` | `Passes::EntityPass::Record` |
| Non-cubic pipeline, back-to-front sort, draws | `Passes::NonCubicPass::Record` |
| `SelectionRenderer::RecordDraw(...)` | `Passes::SelectionPass::Record` |
| *(new)* | `Passes::ShaderPackPass::Record`, the external overlay slot |
| `GUIController::Render(cmd)` | `Passes::GUIPass::Record` |
| End render pass, end command buffer | `VulkanRenderEngine::EndMainPass` |
| `vkQueueSubmit` | `VulkanRenderEngine::SubmitCommandBuffer` |
| `PresentFrame()` | `VulkanRenderEngine::PresentImage` |
| `vkDeviceWaitIdle` + `GUIController::Shutdown` in `RenderThread::Shutdown` | `VulkanRenderEngine::StopRendering` (which also destroys the passes) |

The per-mesh draw code was duplicated between the terrain and non-cubic
loops. It is now `ChunkMeshPass::DrawMesh`, and `BindWorldDescriptorSets`
binds `{cameraSets[frame], textureSet}`.

`RenderThread` now contains only client work: `WaitForTargetFrame`,
`UpdateDeltaTime`, `PollInputs`, `DrawFrame` (GUI update, then
`BeginFrame`/`SubmitFrame`, then frame pacing), `BuildSceneView` and
`BuildProjection`. It no longer calls `vk*` functions.

### One frame, start to finish

```
RenderThread::RunFrame
 ├─ WaitForTargetFrame / UpdateDeltaTime / PollInputs
 └─ DrawFrame
     ├─ GUIController::NewFrame + Update
     ├─ engine->BeginFrame()
     │    ├─ framebufferResized?     → RecreateSwapchain
     │    ├─ shaderReloadRequested?  → ReloadPassShaders (WaitIdle, each pass ReloadShaders)
     │    ├─ vkWaitForFences / vkAcquireNextImageKHR (out of date → return false)
     │    └─ vkResetFences
     └─ engine->SubmitFrame(BuildSceneView())
          ├─ BeginCommandBuffer → UploadCamera → BeginMainPass (clear to sky color)
          ├─ RecordPasses: Terrain, Entities, NonCubic, Selection, ShaderPackOverlay, GUI
          ├─ EndMainPass → SubmitCommandBuffer → PresentImage
          └─ NotifyFramePresented; currentFrame = (currentFrame + 1) % 2
```

---

## 4. Wiring changes

### `VolcanoClient.cpp`

```cpp
Volcano::Engine::Vulkan::VulkanRenderEngine renderEngine(&state);
state.renderEngine = &renderEngine;
renderEngine.Init();                        // was: Init(&state)
Volcano::RenderThread renderer(&state);
...
renderEngine.AttachTextures(textureManager); // was: a raw vkUpdateDescriptorSets on GetTextureSet()
renderEngine.AttachGUI(&textureManager);     // was: GUIController::Init(window, GetRenderPass(), 2, ...)
...
renderer.Shutdown();                         // → renderEngine.StopRendering()
meshingThread.Stop();
renderEngine.Shutdown();                     // was: Cleanup()
```

The shutdown order hasn't changed. First the GPU goes idle and the GUI and
pipelines are released. Then `MeshingThread` is joined. Last, the device is
destroyed, because `ChunkMesher`'s slab buffers are freed there. The **F9**
`ReloadShaders` input action is registered next to `ToggleWireframe`.

### `VulkanInit`

- `Init()` no longer builds pipelines or calls `EntityRenderer::Init()` /
  `SelectionRenderer::Init()`. It still sets up the window, device,
  swapchain, render pass, descriptor sets, command buffers and sync objects.
  `VulkanRenderEngine::Init()` calls it and then creates the passes.
- `CreateGraphicsPipeline` and `CreateNonCubicPipeline` now take a
  `const ShaderLibrary&`. They have idempotent partners,
  `DestroyGraphicsPipeline` and `DestroyNonCubicPipeline`, which
  `TerrainPass` and `NonCubicPass` use (including on reload).
- New shared helper: `CreateShaderModule(shaders, "terrain.vert")`, plus an
  overload that takes raw bytes. It replaces the old private helper and the
  two copies of `CreateShaderModuleFromFile` in `EntityRenderer.cpp` and
  `SelectionRenderer.cpp`. Shader modules are now released on every error
  path. That matters because a broken shader pack is an expected, recoverable
  failure.
- `Cleanup()` no longer shuts down the entity and selection renderers,
  because their passes own them now. It still calls the idempotent
  pipeline-destroy functions as a safety net.

### `EntityRenderer` / `SelectionRenderer`

- `Init(const ShaderLibrary&)` loads shaders through the library.
- New `ReloadPipelines(const ShaderLibrary&)` rebuilds only the pipelines
  and layouts. Meshes, skins and SSBOs stay loaded, so F9 doesn't re-upload
  skins.
- `SelectionRenderer::Shutdown()` now resets its buffer handles, so calling
  it twice is safe.

### Build

`CMakeLists.txt`'s source glob now uses `CONFIGURE_DEPENDS`. Changing
`CMakeLists.txt` makes the committed `build.ninja` re-run CMake on the next
`ninja`, which picks up the new `src/renderer/engine/**.cpp` files. After
that, new `.cpp` files are found automatically. If your build tree doesn't
regenerate for some reason, re-run the CMake configure step once.

---

## 5. External shaders (shader packs)

### Turning a pack on

A pack is a folder under `resources/shaderpacks/`. Select it in
`settings.json`:

```json
{ "Graphics": { "ShaderPack": "Example" } }
```

`""` is the default and means built-in shaders only. From code, call
`state->renderEngine->SetShaderPack("Example")`. It saves the setting and
reloads on the next frame. If the named folder doesn't exist, the client
logs an error and uses the built-in shaders. A stale setting never stops the
client from starting.

### Replacing built-in shaders

Any file named `<shader>.spv` in the pack replaces the built-in
`resources/shaders/<shader>.spv`. Files the pack doesn't include fall back
to the built-in versions.

| Name | Used by | Interface it must keep |
|---|---|---|
| `terrain.vert`, `terrain.frag` | TerrainPass (and wireframe) | `PackedVertex` inputs, `mat4` model push constant, set 0 `CameraUBO`, set 1 texture array |
| `misc.vert`, `misc.frag` | NonCubicPass | `MiscVertex` inputs, same push constant and sets as terrain |
| `entity.vert`, `entity.frag` | EntityPass (placeholder cubes) | `EntityVertex` inputs, set 0 camera, set 1 instance SSBO |
| `entity_textured.vert`, `entity_textured.frag` | EntityPass (humanoids) | `HumanoidModel::Vertex` inputs, `PushConstants`, set 1 skin sampler |
| `selection.vert`, `selection.frag` | SelectionPass | unit-cube positions, `PushConstants{worldMin, worldMax, color}`, set 0 camera |

The simplest way to make an override is to copy the built-in GLSL from
`resources/shaders/`, edit it, and compile it. Vertex inputs, push constants
and descriptor sets must stay the same, because the pipelines are fixed on
the C++ side.

The splash screen and ImGui shaders can't be overridden. The splash is
drawn before the engine exists and uses its own loader. ImGui's pipeline
belongs to its backend.

### The overlay slot: `ShaderPackPass`

A pack can also include `overlay.vert.spv` and `overlay.frag.spv`. These
have no built-in version. When both files are present, `ShaderPackPass`
(named `"ShaderPackOverlay"`) draws one fullscreen triangle after the world
and selection and before the GUI. It is alpha-blended, with no depth test
and no vertex buffer (build the triangle from `gl_VertexIndex`). Without a
pack, the pass records nothing and costs nothing.

Interface, shared by both stages:

```glsl
layout(push_constant) uniform Overlay {
    vec4 viewport; // x: width px, y: height px, z: time s, w: delta time s
    vec4 sky;      // rgb: sky/clear color, a: day brightness (0 midnight .. 1 noon)
    vec4 camera;   // xyz: eye world position, w: 1 lighting on / 0 off
} overlay;
layout(set = 0, binding = 0) uniform CameraUBO { mat4 view; mat4 proj; } camera; // vertex stage
```

The frame is a single subpass, so the overlay **cannot sample the rendered
scene**. It suits tints, vignettes, fades and gradients, not
post-processing. Section 7 covers what reading the scene would take.

### Compiling

The pack takes SPIR-V, not GLSL. `ShaderLibrary` checks the SPIR-V magic
number and word alignment. If a file fails that check, the client logs it
and uses the built-in shader for that file.

```sh
glslangValidator -V overlay.vert -o overlay.vert.spv
glslangValidator -V overlay.frag -o overlay.frag.spv
# or: glslc overlay.frag -o overlay.frag.spv
```

### Live reload and fallback

- **F9**, or `RenderEngine::RequestShaderReload()`, rebuilds every pass's
  pipelines from disk at the start of the next frame, after the GPU is idle.
  Edit a `.spv`, press F9, and see the result without restarting.
- **Fallback levels:**
  - A single *file* that is missing or isn't SPIR-V falls back to the
    built-in shader for that file.
  - If the *driver rejects* a pack's shader (shader module or pipeline
    creation fails), the whole pack is dropped. The client logs the error
    and rebuilds everything with the built-in shaders, both at startup and
    on reload. A bad pack can't crash the client.
  - A failure with *built-in* shaders still throws, the same as before this
    rework.
- Vulkan doesn't check that an override's interface matches the pipeline.
  An override that compiles but declares different vertex inputs or bindings
  is undefined behavior; the validation layers will point it out. Keep the
  interfaces in the table above.

### Example pack

`resources/shaderpacks/Example/` includes GLSL source and compiled `.spv`
for an overlay. It draws a soft vignette that darkens and turns blue at
night and fades out at noon. Set `Graphics.ShaderPack` to `"Example"` to try
it.

---

## 6. Adding your own pass (mods)

```cpp
#include "renderer/engine/RenderEngine.hpp"
#include "renderer/engine/RenderPass.hpp"

class MyOutlinePass : public Volcano::Engine::RenderPass {
public:
    MyOutlinePass() : RenderPass("MyMod.Outline") {}

    void Create(const Volcano::Engine::ShaderLibrary& shaders) override
    {
        // shaders.Load("mymod_outline.frag") resolves pack override → resources/shaders/.
        // Build pipelines against VulkanInit's renderPass here.
    }

    void Record(Volcano::Engine::FrameContext& frame) override
    {
        // Already inside the main render pass with viewport, scissor and camera UBO set.
        // Bind your pipeline and draw into frame.commandBuffer.
    }

    void Destroy() override { /* idempotent teardown */ }
};

// Any time after renderEngine.Init(), on the render thread:
state->renderEngine->InsertPassBefore("GUI", std::make_unique<MyOutlinePass>());
state->renderEngine->FindPass("ShaderPackOverlay")->SetEnabled(false); // turning built-ins off works too
```

If you add a pass after `Init()`, it is created right away. If its
`Create()` throws, the error is logged, the pass isn't added, and
`nullptr` is returned. Pass names must be unique.

---

## 7. Verification, limitations and next steps

**Verified here:**

- Every changed and new translation unit passes `-fsyntax-only` with GCC
  and Clang (`-Wall -Wextra`) against Linux Vulkan, GLM, GLFW, ImGui,
  vk-bootstrap and VMA headers.
- The new `engine/` headers compile standalone.
- A small harness exercised `ShaderLibrary` against the real `resources/`
  tree: built-in lookup, pack override, per-file fallback, missing pack,
  rejecting GLSL source as non-SPIR-V, and pack listing.
- The example overlay compiles to SPIR-V with `glslangValidator`.

**Not verified here:**

- The Windows clang/ninja build (the toolchain paths are `C:/Apps/...`).
- An actual run on a GPU.

Before merging, please build and check that:

- the world, entities, non-cubic blocks, selection outline and GUI look the
  same as before;
- F3, F9 and F11 work;
- resizing works;
- the Example pack's vignette shows up.

**Known limitations and good next steps:**

- **Single subpass.** Passes share one color and depth target, so no pass
  can read what earlier passes drew. Real post-processing needs an offscreen
  color target and a final composite pass. The `RenderPass` and
  `FrameContext` split is the natural place to add pass attachments.
- **Pass-owned pipelines use VulkanInit globals.** `TerrainPass` and
  `NonCubicPass` still read and write `graphicsPipeline`,
  `nonCubicPipeline` and `pipelineLayout` in `VulkanInit`. Moving those
  handles into the pass objects is a follow-up refactor. It isn't needed for
  the abstraction to work.
- **`TextureManager` and `GUIController` are still Vulkan types.** They are
  handed to the engine (`AttachTextures` / `AttachGUI`) instead of being
  driven by client code. Hiding them behind engine-neutral interfaces is the
  remaining step before a second backend would be possible.
- **No in-game shader pack picker.** `ShaderLibrary::ListPacks()` and
  `RenderEngine::SetShaderPack()` are ready for a settings-screen dropdown.
