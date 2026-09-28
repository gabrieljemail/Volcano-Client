#include <iostream>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>
#include "RenderThread.hpp"
#include "gui/GUIController.hpp"
#include "../TickLoop.hpp"
#include "../interaction/InteractionManager.hpp"

using namespace std;

namespace Volcano {
// Declared here rather than including network/NetworkClient.hpp — same
// reasoning as GUIController.cpp's own forward declarations of this
// function's siblings: that header drags in asio.hpp, which on Windows
// pulls in <windows.h>. This one free function is all PollInputs actually
// needs from it.
void SendHeldItemSlot(GlobalState* state, uint8_t slot);
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
    UpdateDeltaTime();
    PollInputs();
    DrawFrame();
    // Both PollInputs (F11, TickLoop's jump check) and DrawFrame (via
    // GUIController::Update()'s Escape check) read this frame's
    // press/release edges above — only clear them now that both have
    // had their chance, not before.
    state->input->EndFrame();
}

void RenderThread::Shutdown()
{
    // Waits for the GPU before destroying anything — see StopRendering.
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
        this_thread::sleep_for(microseconds(remaining - 1500)); // Time remaining - 1.5ms.
    }

    // Spin-wait the final microseconds.
    auto workOffset = microseconds(static_cast<long long>(averageWorkTime * 1000));
    auto spinTarget = nextFrameTarget - workOffset;

    while (steady_clock::now() < spinTarget)
    {
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
    // Dispatches the KeyCallback/CursorPosCallback that feed InputHandler's
    // state, plus window-manager events (resize, focus, close). Used to run
    // in main()'s own separate loop, on the same (GLFW-owning) thread as
    // this one now was a *different* thread — see the class comment. Polling
    // here, immediately before this frame's input is read below, keeps the
    // latency-minimizing order intact: capture as late as possible,
    // immediately before the frame that uses it.
    glfwPollEvents();

    // Bind the close button (and Alt+F4).
    if (glfwWindowShouldClose(state->window))
    {
        state->shouldClose = true;
    }

    // Process inputs.
    state->input->ProcessFrame();

    // Alt+F4 (Windows/Linux), Ctrl+Q (Linux), Cmd+Q (macOS) close the game
    // directly, checked before the Screen-open early return below so it
    // still works while stuck in a menu. Added because relying solely on
    // the OS's own WM_CLOSE handling for Alt+F4 (glfwWindowShouldClose,
    // checked just above) wasn't reliable on every Windows version — see
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

    if (state->input->WasActivated("ToggleWireframe"))
    {
        wireframeMode = !wireframeMode;
    }

    // F9 rebuilds every render pass's pipelines from disk — picks up edited
    // shader pack (or built-in) .spv files without restarting. Applied at
    // the start of the next frame, between frames, by the engine itself.
    if (state->input->WasActivated("ReloadShaders"))
    {
        state->renderEngine->RequestShaderReload();
    }

    // "G" toggles Graphics.Lighting — see SceneView::lightingEnabled, read
    // fresh from config every frame in BuildSceneView, so flipping it
    // here just needs to persist the new value. Gated on chat/screen being
    // closed so typing "g" into the chat box or a text field doesn't also
    // toggle it.
    if (!GUIController::IsChatInputOpen() && !GUIController::IsScreenOpen()
        && state->input->WasActivated("ToggleLighting"))
    {
        bool lightingEnabled = std::get<bool>(state->config->Get("Graphics.Lighting", true));
        state->config->Set("Graphics.Lighting", !lightingEnabled);
        state->config->Save();
    }

    // While a Screen (connect screen, pause menu, death screen, ...) or the
    // chat input box is open, ImGui owns the mouse/keyboard — don't let them
    // also fly the camera or move the player underneath it. TickLoop::Advance
    // still runs every frame regardless (see its own inputAllowed parameter)
    // — gravity/collision and applying the server's teleports (e.g. a
    // respawn) must keep going even with a screen up, or the player freezes
    // mid-air and a pending teleport never lands until the screen closes.
    bool inputAllowed = !GUIController::IsScreenOpen() && !GUIController::IsChatInputOpen();

    // Hotbar slot selection ("1".."9" — see their registration in
    // VolcanoClient.cpp). Gated the same way "G"/ToggleLighting is: typing a
    // digit into chat or a text field must not also swap the held slot.
    // Applied immediately to local state (read straight off GlobalState by
    // GUIController::RenderHotbarAndArmor, no round trip needed for the HUD
    // to update) and reported to the server so its own notion of the held
    // slot — which future block-interaction packets need to agree with —
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

    if (inputAllowed)
    {
        // Mouse sensitivity is applied entirely at the axis level (see the
        // Camera.X/Y registration in VolcanoClient.cpp) — pass 1.0f here so
        // there's exactly one sensitivity knob, not two multiplied together.
        float dx = state->input->GetAxis("Camera.X");
        float dy = state->input->GetAxis("Camera.Y");
        state->player->camera.ApplyMouseDelta(dx, dy, 1.0f);
    }

    // Mainhand/offhand interaction (attack; block breaking/placing once
    // added) — same inputAllowed gate as Hotbar/Camera above.
    state->interaction->Update(inputAllowed);

    // Gravity/collision/movement run on NetworkThread's own steady 20Hz
    // clock now (see the tick-loop plan), not here — this just publishes
    // what Tick() needs from this frame's input. Jump/JumpHold are gated by
    // inputAllowed right at this read site (a screen/chat box being open
    // means a jump press is never even latched, not latched-and-ignored),
    // matching TickLoop::RequestJump's own comment; Move/Sneak/Sprint are
    // published unconditionally, same as before — FixedStep() zeroes the
    // movement axes itself when input isn't allowed.
    if (inputAllowed && state->input->WasActivated("Jump"))
    {
        state->tickLoop->RequestJump();
    }
    state->tickLoop->PublishInput(
        inputAllowed,
        state->input->GetAxis("Move.Forward"),
        state->input->GetAxis("Move.Right"),
        state->input->IsActive("Sneak"),
        state->input->IsActive("Sprint"),
        inputAllowed && state->input->IsActive("JumpHold"));
}

void RenderThread::DrawFrame()
{
    // Time the current frame.
    auto workStart = chrono::steady_clock::now();

    // Update GUI with new frame
    GUIController::NewFrame();
    GUIController::Update(frameDeltaTime);

    // BeginFrame handles any pending resize/shader reload and acquires an
    // output image; false means this frame is skipped (stale or minimized
    // swapchain). The SceneView is only built after it, since a resize there
    // changes the aspect ratio the projection needs.
    Engine::RenderEngine* engine = state->renderEngine;
    if (engine->BeginFrame())
    {
        engine->SubmitFrame(BuildSceneView());
    }

    auto workEnd = chrono::steady_clock::now();

    // Schedule the next frame.
    double frameWorkTime = std::chrono::duration<double, std::milli>(workEnd - workStart).count();
    averageWorkTime = (averageWorkTime * 0.9) + (frameWorkTime * 0.1);
    nextFrameTarget += std::chrono::microseconds(static_cast<long long>(targetFrameTime * 1000));
}

namespace {

// Vanilla's own day-time convention: 0/24000 = dawn, 6000 = noon (brightest),
// 12000 = dusk, 18000 = midnight (darkest) — see GlobalState::dayTimeTicks.
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

// Sampled between these two extremes by DayBrightness() above — no weather
// tinting yet (see ISSUES.mdx; that needs particle rendering first).
constexpr glm::vec3 SKY_COLOR_NIGHT(0.015f, 0.015f, 0.035f);
constexpr glm::vec3 SKY_COLOR_DAY(0.52941f, 0.80784f, 0.92157f);

} // namespace

Engine::SceneView RenderThread::BuildSceneView()
{
    Engine::SceneView scene;

    glm::vec3 renderPosition = state->tickLoop->GetRenderPosition();
    scene.view = state->player->camera.GetViewMatrix(renderPosition);
    scene.proj = BuildProjection(state->renderEngine->GetExtent());
    scene.cameraPosition = state->player->camera.GetEyePosition(renderPosition);

    scene.dayBrightness = DayBrightness(state->dayTimeTicks.load());
    scene.skyColor = glm::mix(SKY_COLOR_NIGHT, SKY_COLOR_DAY, scene.dayBrightness);

    scene.time = chrono::duration<float>(chrono::steady_clock::now() - startTime).count();
    scene.deltaTime = frameDeltaTime;

    scene.lightingEnabled = std::get<bool>(state->config->Get("Graphics.Lighting", true));
    scene.wireframe = wireframeMode;
    return scene;
}

glm::mat4 RenderThread::BuildProjection(const Engine::RenderExtent& extent)
{
    // A minimized window can report a zero-height extent for a frame.
    float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height > 0 ? extent.height : 1);
    float baseFov = static_cast<float>(std::get<uint32_t>(state->config->Get("Graphics.FOV", uint32_t{100})));

    // Flying doesn't exist yet (no fly toggle — same placeholder
    // GUIController's movement-state panel uses) so only sprinting drives
    // this for now; whoever adds flight just needs to flip isFlying here,
    // the easing below already handles either.
    constexpr bool isFlying = false;
    bool boosted = isFlying || state->input->IsActive("Sprint");
    float targetFovOffset = boosted
        ? static_cast<float>(std::get<uint32_t>(state->config->Get("Graphics.FOVEffects", uint32_t{5})))
        : 0.0f;

    // Framerate-independent exponential ease toward the target offset every
    // frame instead of snapping the instant a sprint starts/stops — see
    // currentFovOffset's own comment. Also where a future FOV *decrease*
    // (aiming down sights, etc.) would plug in: just another signed target
    // fed into the same easing.
    constexpr float FOV_EASE_RATE = 8.0f; // higher = snappier
    currentFovOffset = glm::mix(currentFovOffset, targetFovOffset,
        1.0f - std::exp(-FOV_EASE_RATE * frameDeltaTime));

    // Vulkan's clip space has Y pointing down — flip it so the image lands
    // right-side-up.
    glm::mat4 p = glm::perspective(glm::radians(baseFov + currentFovOffset), aspect, 0.05f, 1000.0f);
    p[1][1] *= -1.0f;
    return p;
}

}
