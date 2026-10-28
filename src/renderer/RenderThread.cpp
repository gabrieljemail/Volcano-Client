#include <algorithm>
#include <iostream>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>
#include "../Math.hpp"
#include "RenderThread.hpp"
#include "gui/GUIController.hpp"
#include "../TickLoop.hpp"
#include "../ModLoader.hpp"
#include "../DropKey.hpp"
#include "../interaction/InteractionManager.hpp"
#include "../platform/OS.hpp"
#include "../resources/ResourceReloader.hpp"

using namespace std;

namespace Volcano {
// Declared here rather than including network/NetworkClient.hpp - same
// reasoning as GUIController.cpp's own forward declarations of this
// function's siblings: that header drags in asio.hpp, which on Windows
// pulls in <windows.h>. This one free function is all PollInputs actually
// needs from it.
void SendHeldItemSlot(GlobalState* state, uint8_t slot);
void SendPlayerAction(GlobalState* state, DigStatus status, glm::ivec3 position, glm::ivec3 face);
}

namespace Volcano {

RenderThread::RenderThread(Volcano::GlobalState* globalState) : state(globalState)
{
    if (state && state->targetFPS > 0)
    {
        targetFrameTime = 1000.0 / static_cast<double>(state->targetFPS);
    } else
    {
        targetFrameTime = 1000.0 / 60.0;
    }

    lastFrameTime = chrono::steady_clock::now();
    frameStartTime = lastFrameTime;
    startTime = lastFrameTime;
    nextFrameTarget = frameStartTime + chrono::microseconds(static_cast<long long>(targetFrameTime * 1000));
}

RenderThread::~RenderThread()
{}

void RenderThread::RunFrame()
{
    WaitForTargetFrame();
    auto workStart = chrono::steady_clock::now();
    UpdateDeltaTime();
    PollInputs();
    DrawFrame();
    // Both PollInputs (F11, TickLoop's jump check) and DrawFrame (via
    // GUIController::Update()'s Escape check) read this frame's
    // press/release edges above
    state->input->EndFrame();

    // Everything above except pacing, split into real CPU work and time
    // blocked on the GPU (fence/acquire, reported by the engine).
    // Debug HUD only; skipped while it's hidden.
    if (!state->debugHudVisible) return;
    float totalMs = chrono::duration<float, std::milli>(chrono::steady_clock::now() - workStart).count();
    float waitMs = state->timings.waitRawMs.load();
    constexpr float smoothing = 0.05f;
    auto smooth = [](std::atomic<float>& field, float sample)
    {
        field.store(field.load() + (sample - field.load()) * smoothing);
    };
    smooth(state->timings.renderCpuMs, std::max(0.0f, totalMs - waitMs));
    smooth(state->timings.gpuWaitMs, waitMs);
}

void RenderThread::Shutdown()
{
    // Waits for the GPU before destroying anything
    state->renderEngine->StopRendering();
}

void RenderThread::WaitForTargetFrame()
{
    using namespace chrono;

    // Calculate time remaining until next target.
    auto now = steady_clock::now();
    auto remaining = duration_cast<microseconds>(nextFrameTarget - now).count();

    // Sleep for the majority of the frame time.
    // Add safety padding to make sure the scheduler doesn't make us oversleep.
    if (remaining > 2000)
    {
        Platform::OS::PreciseSleep(remaining - 1500); // Time remaining - 1.5ms.
    }

    // Spin-wait the final microseconds.
    auto workOffset = microseconds(static_cast<long long>(averageWorkTime * 1000));
    auto spinTarget = nextFrameTarget - workOffset;

    while (steady_clock::now() < spinTarget)
    {
        // Note: Even yield() can be off because this hands off to the scheduler,
        // which can take as long as it wants. It's still better than sleep though.
        // In the future, we can replace yield() with a no-op like a useless while loop.
        // However, yield() resets the execution timer to fit all of our operations, while
        // the no-op method does not and might cause the timer to interrupt mid-frame.
        // Ideally, we want to keep interrupts off-frame, except for inputs.
        this_thread::yield();
    }
}

void RenderThread::UpdateDeltaTime()
{
    auto now = chrono::steady_clock::now();
    frameDeltaTime = chrono::duration<float>(now - lastFrameTime).count();
    lastFrameTime = now;
}

void RenderThread::PollInputs()
{
    state->input->PollRawMouse();
    glfwPollEvents();

    // Bind the close button (and Alt+F4).
    if (glfwWindowShouldClose(state->window))
    {
        state->shouldClose = true;
    }

    // Process inputs.
    state->input->ProcessFrame();

    if (state->modLoader != nullptr && state->modLoader->HasHandlers(EventType::KEY_PRESSED))
    {
        for (int key = 0; key <= GLFW_KEY_LAST; key++)
        {
            if (!state->input->IsKeyPressed(key)) continue;
            VC_EventData data{ sizeof(VC_EventData), nullptr, key, 0, 0, 0 };
            state->modLoader->Fire(EventType::KEY_PRESSED, &data);
        }
    }

    // Alt+F4 (Windows/Linux), Ctrl+Q (Linux), Cmd+Q (macOS) close the game
    // directly, checked before the Screen-open early return below so it
    // still works while stuck in a menu. Added because relying solely on
    // the OS's own WM_CLOSE handling for Alt+F4 (glfwWindowShouldClose,
    // checked just above) wasn't reliable on every Windows version - see
    // the Pause screen's own comment for the underlying cursor/focus fight.
#if defined(_WIN32) || defined(__linux__)
    bool altDown = state->input->IsKeyDown(GLFW_KEY_LEFT_ALT) || state->input->IsKeyDown(GLFW_KEY_RIGHT_ALT);
    if (altDown && state->input->IsKeyPressed(GLFW_KEY_F4)) state->shouldClose = true;
#endif
#if defined(__linux__)
    bool ctrlDown = state->input->IsKeyDown(GLFW_KEY_LEFT_CONTROL) || state->input->IsKeyDown(GLFW_KEY_RIGHT_CONTROL);
    if (ctrlDown && state->input->IsKeyPressed(GLFW_KEY_Q)) state->shouldClose = true;
#endif
#if defined(__APPLE__)
    bool superDown = state->input->IsKeyDown(GLFW_KEY_LEFT_SUPER) || state->input->IsKeyDown(GLFW_KEY_RIGHT_SUPER);
    if (superDown && state->input->IsKeyPressed(GLFW_KEY_Q)) state->shouldClose = true;
#endif

    // F11 cycles window modes regardless of whether a Screen is open, same
    // as it would in most games.
    if (state->input->IsKeyPressed(GLFW_KEY_F11))
    {
        state->renderEngine->ToggleWindowMode();
    }
    if (state->input->IsKeyPressed(GLFW_KEY_F2))
    {
        state->renderEngine->RequestScreenshot();
    }
    // Picks up Window.* changes from a Settings screen (no-op otherwise).
    state->renderEngine->ApplyWindowSettings();

    // F3 toggles the debug HUD on release, like vanilla, so F3+Z (wireframe)
    // can be used as a combo without also flipping the HUD.
    if (state->input->IsKeyDown(GLFW_KEY_F3) && state->input->IsKeyPressed(GLFW_KEY_Z)
        && !GUIController::IsChatInputOpen())
    {
        wireframeMode = !wireframeMode;
        f3ComboUsed = true;
    }
    // F3+T reloads resource packs (applied at the start of DrawFrame).
    if (state->input->IsKeyDown(GLFW_KEY_F3) && state->input->IsKeyPressed(GLFW_KEY_T)
        && !GUIController::IsChatInputOpen())
    {
        reloadResourcesRequested = true;
        f3ComboUsed = true;
    }
    if (state->input->IsActive("ToggleDebugHud"))
    {
        if (!f3ComboUsed) state->debugHudVisible = !state->debugHudVisible;
        f3ComboUsed = false;
    }

    // Controls.ToggleSprint: a Sprint press latches sprinting on/off instead
    // of needing the key held. Read every frame, same as Graphics.Lighting
    // below - this isn't hot enough to bother caching.
    bool toggleSprint = std::get<bool>(state->config->Get("Controls.ToggleSprint", false));
    if (toggleSprint && state->input->WasActivated("Sprint"))
    {
        sprintToggled = !sprintToggled;
    }

    // F9 rebuilds every render pass's pipelines from disk
    if (state->input->WasActivated("ReloadShaders"))
    {
        state->renderEngine->RequestShaderReload();
    }

    // "G" toggles Graphics.Lighting
    if (!GUIController::IsChatInputOpen() && !GUIController::IsScreenOpen()
        && state->input->WasActivated("ToggleLighting"))
    {
        bool lightingEnabled = std::get<bool>(state->config->Get("Graphics.Lighting", true));
        state->config->Set("Graphics.Lighting", !lightingEnabled);
        state->config->Save();
    }

    if (!GUIController::IsChatInputOpen() && !GUIController::IsScreenOpen()
        && state->input->WasActivated("CyclePerspective"))
    {
        state->player->camera.CyclePerspective();
    }

    // While a Screen (connect screen, pause menu, death screen, ...) or the
    // chat input box is open, ImGui owns the mouse/keyboard - don't let them
    // also fly the camera or move the player underneath it. TickLoop::Advance
    // still runs every frame regardless (see its own inputAllowed parameter)
    // - gravity/collision and applying the server's teleports (e.g. a
    // respawn) must keep going even with a screen up, or the player freezes
    // mid-air and a pending teleport never lands until the screen closes.
    bool inputAllowed = !GUIController::IsScreenOpen() && !GUIController::IsChatInputOpen();

    // Hotbar slot selection ("1".."9" - see their registration in
    // VolcanoClient.cpp). Gated the same way "G"/ToggleLighting is: typing a
    // digit into chat or a text field must not also swap the held slot.
    // Applied immediately to local state (read straight off GlobalState by
    // GUIController::RenderHotbarAndArmor, no round trip needed for the HUD
    // to update) and reported to the server so its own notion of the held
    // slot - which future block-interaction packets need to agree with -
    // doesn't silently drift from what's shown locally.
    if (inputAllowed)
    {
        for (int slot = 0; slot < 9; slot++)
        {
            if (!state->input->WasActivated("Hotbar." + std::to_string(slot))) continue;

            {
                std::lock_guard<std::mutex> lock(state->inventory.mutex);
                state->inventory.selectedHotbarSlot = static_cast<uint8_t>(slot);
            }
            SendHeldItemSlot(state, static_cast<uint8_t>(slot));
            break; // Two number keys can't both have been pressed the same frame in practice; stop at the first.
        }
    }

    // Drop key: a tap drops one item, holding it (or Ctrl) drops the stack
    static DropKeyTracker dropKey;
    if (inputAllowed && state->localGameMode.load() != 3)
    {
        bool ctrlDown = state->input->IsKeyDown(GLFW_KEY_LEFT_CONTROL) || state->input->IsKeyDown(GLFW_KEY_RIGHT_CONTROL);
        DropKeyTracker::Result drop = dropKey.Update(state->input->IsActive("DropItem"),
            state->input->IsActive("DropItemHold"), state->input->IsActive("DropItemRelease"), ctrlDown, frameDeltaTime);
        if (drop != DropKeyTracker::Result::NONE)
        {
            bool wholeStack = drop == DropKeyTracker::Result::STACK;
            bool hadItem = false;
            {
                std::lock_guard<std::mutex> lock(state->inventory.mutex);
                ItemStack& held = state->inventory.hotbar.Get(state->inventory.selectedHotbarSlot);
                hadItem = !held.IsEmpty();
                if (hadItem)
                {
                    if (wholeStack || held.count <= 1) held = ItemStack{};
                    else held.count--;
                }
            }
            if (hadItem) SendPlayerAction(state, wholeStack ? DigStatus::DropItemStack : DigStatus::DropItem, glm::ivec3(0), glm::ivec3(0));
        }
    }
    else
    {
        dropKey.Reset();
    }

    if (inputAllowed)
    {
        // Mouse sensitivity is applied entirely at the axis level (see the
        // Camera.X/Y registration in VolcanoClient.cpp) - pass 1.0f here so
        // there's exactly one sensitivity knob, not two multiplied together.
        float dx = state->input->GetAxis("Camera.X");
        float dy = state->input->GetAxis("Camera.Y");
        float sensitivity = static_cast<float>(
            std::get<uint32_t>(state->config->Get("Controls.Mouse.Sensitivity", uint32_t{100}))) / 100.0f;
        state->player->camera.ApplyMouseDelta(dx, dy, sensitivity);
    }

    // Mainhand/offhand interaction (attack; block breaking/placing once
    // added) - same inputAllowed gate as Hotbar/Camera above.
    state->interaction->Update(inputAllowed);

    // Publish this frame's input, then drive TickLoop with it immediately -
    // gravity/collision/movement run right here (see TickLoop's class
    // comment). Jump is gated by inputAllowed at this read site; Move/Sneak/
    // Sprint publish unconditionally, since FixedStep() zeroes them itself.
    if (inputAllowed && state->input->WasActivated("Jump"))
    {
        state->tickLoop->RequestJump();
    }
    state->tickLoop->PublishInput(
        inputAllowed,
        state->input->GetAxis("Move.Forward"),
        state->input->GetAxis("Move.Right"),
        state->input->IsActive("Sneak"),
        toggleSprint ? sprintToggled : state->input->IsActive("Sprint"),
        inputAllowed && state->input->IsActive("JumpHold"));
    state->tickLoop->Tick();
}

void RenderThread::DrawFrame()
{
    // Time the current frame.
    auto workStart = chrono::steady_clock::now();

    // Safe point: no ImGui frame open and no command buffer recording.
    if (reloadResourcesRequested || ResourceReloader::ConsumeRequest())
    {
        reloadResourcesRequested = false;
        ResourceReloader::Reload(state);

        // The reload took seconds; don't feed that into physics or pacing.
        auto reloaded = chrono::steady_clock::now();
        lastFrameTime = reloaded;
        frameDeltaTime = 0.0f;
        nextFrameTarget = reloaded;
        workStart = reloaded;
    }

    // Update GUI with new frame
    GUIController::NewFrame();
    GUIController::Update(frameDeltaTime);
    if (state->modLoader != nullptr) state->modLoader->Fire(EventType::FRAME_BEGIN);

    // BeginFrame handles any pending resize/shader reload and acquires an
    // output image; false means this frame is skipped (stale or minimized
    // swapchain). The SceneView is only built after it, since a resize there
    // changes the aspect ratio the projection needs.
    Engine::RenderEngine* engine = state->renderEngine;
    if (engine->BeginFrame())
    {
        engine->SubmitFrame(BuildSceneView());
    }
    else
    {
        // Close the ImGui frame NewFrame opened, or the next one asserts.
        GUIController::DiscardFrame();
    }

    auto workEnd = chrono::steady_clock::now();

    // Schedule the next frame.
    double frameWorkTime = std::chrono::duration<double, std::milli>(workEnd - workStart).count();
    averageWorkTime = (averageWorkTime * 0.9) + (frameWorkTime * 0.1);
    // Re-read each frame so the Max FPS setting applies live.
    uint16_t targetFPS = state->targetFPS;
    targetFrameTime = 1000.0 / static_cast<double>(targetFPS > 0 ? targetFPS : 60);
    nextFrameTarget += std::chrono::microseconds(static_cast<long long>(targetFrameTime * 1000));

    // Fell more than a frame behind (a hitch, or the limit was just raised):
    // resync instead of rushing frames out to catch up.
    if (nextFrameTarget < workEnd)
    {
        nextFrameTarget = workEnd + std::chrono::microseconds(static_cast<long long>(targetFrameTime * 1000));
    }
}

namespace {

// Vanilla's own day-time convention: 0/24000 = dawn, 6000 = noon (brightest),
// 12000 = dusk, 18000 = midnight (darkest) - see GlobalState::dayTimeTicks.
// A single cosine gives a smooth day/night gradient without hardcoding a
// multi-segment sunrise/sunset table: brightness is 1.0 exactly at noon,
// 0.0 exactly at midnight, and 0.5 at both twilight points (dawn/dusk),
// which is a reasonable sky brightness for that time even though it's not
// vanilla's real (much more elaborate) celestial-angle curve.
float DayBrightness(int64_t dayTimeTicks)
{
    constexpr double TWO_PI = 6.283185307179586;
    double t = static_cast<double>(dayTimeTicks) / 24000.0;
    return static_cast<float>((std::cos(TWO_PI * (t - 0.25)) + 1.0) * 0.5);
}

// Sky gradient endpoints, blended by DayBrightness() above. Day values are
// vanilla's plains sky (0x78A7FF) and its paler fog/horizon color. No
// weather tinting yet.
constexpr glm::vec3 SKY_TOP_NIGHT(0.004f, 0.004f, 0.016f);
constexpr glm::vec3 SKY_TOP_DAY(0.471f, 0.655f, 1.0f);
constexpr glm::vec3 SKY_HORIZON_NIGHT(0.02f, 0.02f, 0.05f);
constexpr glm::vec3 SKY_HORIZON_DAY(0.753f, 0.847f, 1.0f);

// C++ mirror of terrain.frag's NotGamma
float NotGamma(float level)
{
    float inverted = 1.0f - level;
    return 1.0f - (inverted * inverted * inverted * inverted);
}

// Vanilla's GameRenderer.bobView: a side-to-side sway and a dip with each
// step, a roll with the sway and a slight nod.
glm::mat4 ViewBobMatrix(const TickLoop::ViewBob& bob)
{
    float phase = -bob.walkDistance * glm::pi<float>();
    float h = bob.bob;
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(std::sin(phase) * h * 0.5f, -std::abs(std::cos(phase) * h), 0.0f));
    m = glm::rotate(m, glm::radians(std::sin(phase) * h * 3.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    m = glm::rotate(m, glm::radians(std::abs(std::cos(phase - 0.2f) * h) * 5.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    return m;
}

} // namespace

Engine::SceneView RenderThread::BuildSceneView()
{
    Engine::SceneView scene;

    glm::vec3 renderPosition = state->tickLoop->GetRenderPosition();

    // Body View: the camera rides higher so it sits at the model's eyes. Render-only; reach and
    // raycasts still use the real eye the server checks.
    glm::vec3 cameraBase = renderPosition;

    // The tick thread eases the eye height 20 times a second; smoothing it per frame keeps the sneak dip from stepping.
    {
        static float smoothEye = -1.0f;
        static std::chrono::steady_clock::time_point lastEye = std::chrono::steady_clock::now();
        auto nowEye = std::chrono::steady_clock::now();
        float deltaEye = std::min(std::chrono::duration<float>(nowEye - lastEye).count(), 0.1f);
        lastEye = nowEye;
        float eyeNow = state->player->camera.eyeHeight.load(std::memory_order_relaxed);
        smoothEye = smoothEye < 0.0f ? eyeNow : Math::Damp(smoothEye, eyeNow, 30.0f, deltaEye);
        cameraBase.y += smoothEye - eyeNow;
    }
    if (state->player->camera.IsFirstPerson() && std::get<bool>(state->config->Get("Graphics.BodyView", false)))
    {
        constexpr float STANDING_EYE = 1.62f;
        constexpr float BODY_VIEW_EYE = 1.85f;
        float eye = state->player->camera.eyeHeight.load(std::memory_order_relaxed);
        cameraBase.y += (BODY_VIEW_EYE - STANDING_EYE) * std::min(eye / STANDING_EYE, 1.0f);
    }
    scene.view = state->player->camera.GetViewMatrix(cameraBase);
    scene.proj = BuildProjection(state->renderEngine->GetExtent());
    scene.cameraPosition = state->player->camera.GetCameraPosition(cameraBase);

    // View bobbing, applied in front of the camera as vanilla's bobView does.
    // The camera position then comes from the bobbed view, so passes that draw
    // camera-relative (particles, clouds) stay lined up with the terrain.
    if (std::get<bool>(state->config->Get("Graphics.ViewBobbing", true)))
    {
        scene.viewBob = ViewBobMatrix(state->tickLoop->GetViewBob());
        scene.view = scene.viewBob * scene.view;
        scene.cameraPosition = glm::vec3(glm::inverse(scene.view)[3]);
    }

    // Read once and shared between skyColor below and the UBO fields further
    // down (VulkanRenderEngine::SubmitFrame), rather than two separate config
    // lookups + a second DayBrightness call for what has to be the same
    // frame's values anyway.
    bool lightingEnabled = std::get<bool>(state->config->Get("Graphics.Lighting", true));
    float brightnessFactor = static_cast<float>(
        std::get<uint32_t>(state->config->Get("Graphics.Brightness", uint32_t{50}))) / 100.0f;
    // See terrain.frag's LightCurve: 3 = vanilla, 0 = linear.
    float lightFalloff = static_cast<float>(
        std::get<uint32_t>(state->config->Get("Graphics.LightFalloff", uint32_t{3})));

    int64_t dayTime = state->dayTimeTicks.load();
    scene.dayBrightness = DayBrightness(dayTime);
    scene.brightnessFactor = brightnessFactor;
    scene.lightFalloff = lightFalloff;
    scene.specularStrength = static_cast<float>(
        std::get<uint32_t>(state->config->Get("Graphics.Specular", uint32_t{50}))) / 100.0f;

    // Sun rises in the east (+X), is overhead at noon (6000), sets in the
    // west; the moon is always opposite. Phase advances once per day.
    constexpr double TWO_PI = 6.283185307179586;
    int64_t timeOfDay = ((dayTime % 24000) + 24000) % 24000;
    double sunAngle = TWO_PI * static_cast<double>(timeOfDay) / 24000.0;
    scene.sunDirection = glm::vec3(static_cast<float>(std::cos(sunAngle)), static_cast<float>(std::sin(sunAngle)), 0.0f);
    int64_t day = dayTime >= 0 ? dayTime / 24000 : (dayTime - 23999) / 24000;
    scene.moonPhase = static_cast<int>(((day % 8) + 8) % 8);

    // Vanilla's terrain fog: ends at the render distance, fading in over the
    // last tenth of it (4-64 blocks).
    float renderDistanceBlocks = static_cast<float>(
        std::get<uint32_t>(state->config->Get("Graphics.RenderDistance", uint32_t{8}))) * 16.0f;
    scene.fogEnd = renderDistanceBlocks;
    scene.fogStart = renderDistanceBlocks - std::clamp(renderDistanceBlocks / 10.0f, 4.0f, 64.0f);

    // Vanilla's hardcoded per-face shade (LevelRenderer::getShade), stored
    // as a 0-100 percentage like Brightness above rather than a raw float -
    // Config has no float alternative (see ConfigValue). Face defaults are
    // Up=100, Down=40, North/South=80, East/West=60.
    scene.shadeTop = static_cast<float>(std::get<uint32_t>(state->config->Get("Graphics.Shading.Top", uint32_t{100}))) / 100.0f;
    scene.shadeBottom = static_cast<float>(std::get<uint32_t>(state->config->Get("Graphics.Shading.Bottom", uint32_t{40}))) / 100.0f;
    scene.shadeNorthSouth = static_cast<float>(std::get<uint32_t>(state->config->Get("Graphics.Shading.NorthSouth", uint32_t{80}))) / 100.0f;
    scene.shadeEastWest = static_cast<float>(std::get<uint32_t>(state->config->Get("Graphics.Shading.EastWest", uint32_t{60}))) / 100.0f;

    // Same γ/NotGamma blend terrain.frag applies to per-block lighting, so
    // raising Graphics.Brightness lifts the night sky too instead of only
    // affecting terrain
    float skyColorBrightness = lightingEnabled
        ? glm::mix(scene.dayBrightness, NotGamma(scene.dayBrightness), brightnessFactor)
        : scene.dayBrightness;
    // skyColor is the horizon: the clear color, fog color and gradient base.
    // The constants are sRGB (as picked); the swapchain is sRGB too, so
    // shaders work in linear and the output gets encoded once on display.
    auto toLinear = [](glm::vec3 srgb) { return glm::pow(srgb, glm::vec3(2.2f)); };
    scene.skyColor = toLinear(glm::mix(SKY_HORIZON_NIGHT, SKY_HORIZON_DAY, skyColorBrightness));
    scene.skyTopColor = toLinear(glm::mix(SKY_TOP_NIGHT, SKY_TOP_DAY, skyColorBrightness));

    scene.time = chrono::duration<float>(chrono::steady_clock::now() - startTime).count();
    scene.deltaTime = frameDeltaTime;

    // Not just !worldReady: a mid-session Respawn drops worldReady briefly,
    // and the panorama shouldn't flash up for that (phase stays Playing).
    scene.menuBackground = !state->worldReady.load()
        && state->connectionPhase.load() != ConnectionPhase::Playing;
    scene.lightingEnabled = lightingEnabled;
    scene.wireframe = wireframeMode;
    scene.indirectDraw = std::get<bool>(state->config->Get("Graphics.IndirectDraw", true));
    return scene;
}

glm::mat4 RenderThread::BuildProjection(const Engine::RenderExtent& extent)
{
    // A minimized window can report a zero-height extent for a frame.
    float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height > 0 ? extent.height : 1);
    float baseFov = static_cast<float>(std::get<uint32_t>(state->config->Get("Graphics.FOV", uint32_t{80})));

    // Flying doesn't exist yet (no fly toggle - same placeholder
    // GUIController's movement-state panel uses) so only sprinting drives
    // this for now; whoever adds flight just needs to flip isFlying here,
    // the easing below already handles either.
    constexpr bool isFlying = false;
    bool boosted = isFlying || state->tickLoop->GetMovementSnapshot().sprinting;
    float targetFovOffset = boosted
        ? static_cast<float>(std::get<uint32_t>(state->config->Get("Graphics.FOVEffects", uint32_t{10})))
        : 0.0f;

    // Framerate-independent exponential ease toward the target offset every
    // frame instead of snapping the instant a sprint starts/stops - see
    // currentFovOffset's own comment. Also where a future FOV *decrease*
    // (aiming down sights, etc.) would plug in: just another signed target
    // fed into the same easing.
    constexpr float FOV_EASE_RATE = 8.0f; // higher = snappier
    currentFovOffset = glm::mix(currentFovOffset, targetFovOffset,
        1.0f - std::exp(-FOV_EASE_RATE * frameDeltaTime));

    // Vulkan's clip space has Y pointing down
    glm::mat4 p = glm::perspective(glm::radians(baseFov + currentFovOffset), aspect, 0.05f, 1000.0f);
    p[1][1] *= -1.0f;
    return p;
}

}
