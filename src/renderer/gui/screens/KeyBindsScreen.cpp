#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <imgui.h>
#include "KeyBindsScreen.hpp"
#include "../GUIController.hpp"
#include "../../../Config.hpp"
#include "../../../GlobalState.hpp"
#include "../../../InputHandler.hpp"
#include "../../../KeyBindings.hpp"

namespace Volcano::GUI {

namespace {

constexpr float ROW_WIDTH = 520.0f;
constexpr float KEY_BUTTON_WIDTH = 170.0f;
constexpr float RESET_BUTTON_WIDTH = 64.0f;
constexpr float LIST_HEIGHT = 320.0f;

// The screen's state, shared by its callbacks.
struct ScreenState {
    std::string listening; // Id of the binding waiting for a key; empty when none.
    int skipFrames = 0;    // The click that started listening must not bind itself.
};

// While a binding listens, takes the next key or mouse button for it.
void CaptureKey(GlobalState* state, ScreenState& screen)
{
    if (screen.listening.empty()) return;
    if (screen.skipFrames > 0)
    {
        screen.skipFrames--;
        return;
    }

    InputHandler& input = *state->input;
    if (input.IsKeyPressed(GLFW_KEY_ESCAPE))
    {
        screen.listening.clear();
        return;
    }
    if (input.IsKeyPressed(GLFW_KEY_BACKSPACE) || input.IsKeyPressed(GLFW_KEY_DELETE))
    {
        KeyBindings::Rebind(*state->config, input, screen.listening, "None");
        screen.listening.clear();
        return;
    }
    for (uint16_t code : KeyBindings::BindableCodes())
    {
        if (!input.IsKeyPressed(code)) continue;
        KeyBindings::Rebind(*state->config, input, screen.listening, KeyBindings::KeyName(code));
        screen.listening.clear();
        return;
    }
}

void DrawKeyBinds(GlobalState* state, ScreenState& screen, const std::function<void()>& onDone)
{
    Config& config = *state->config;
    const float windowWidth = ImGui::GetWindowWidth();
    const float left = (windowWidth - ROW_WIDTH) * 0.5f;

    CaptureKey(state, screen);

    ImGui::SetCursorPosX(left);
    ImGui::BeginChild("##keybinds", ImVec2(ROW_WIDTH, LIST_HEIGHT), false, ImGuiWindowFlags_NoBackground);

    std::string category;
    for (const KeyBindingDef& def : KeyBindings::All())
    {
        if (def.category != category)
        {
            category = def.category;
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextDisabled("%s", category.c_str());
        }

        ImGui::PushID(def.id.c_str());
        GUIController::AlignTextToControl();
        ImGui::TextUnformatted(def.label.c_str());

        const bool listening = screen.listening == def.id;
        std::vector<std::string> conflicts = KeyBindings::Conflicts(config, def.id);
        std::string keyName = KeyBindings::GetKey(config, def.id);

        ImGui::SameLine(ROW_WIDTH - ImGui::GetStyle().ScrollbarSize - KEY_BUTTON_WIDTH - RESET_BUTTON_WIDTH - 8.0f);
        std::string caption = listening ? "> Press a key <" : keyName;
        if (GUIController::DrawButton(caption + "##key", KEY_BUTTON_WIDTH) && !listening)
        {
            screen.listening = def.id;
            screen.skipFrames = 1;
        }

        // A key shared with another control gets a red outline and says who else uses it.
        if (!conflicts.empty() && !listening)
        {
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                IM_COL32(255, 90, 90, 255), 6.0f, 0, 2.0f);
        }
        if (ImGui::IsItemHovered() && !conflicts.empty() && !listening)
        {
            std::string shared = "Also used by:";
            for (const std::string& label : conflicts) shared += "\n  " + label;
            ImGui::SetTooltip("%s", shared.c_str());
        }

        ImGui::SameLine(0.0f, 8.0f);
        if (GUIController::DrawButton("Reset", RESET_BUTTON_WIDTH))
        {
            KeyBindings::Reset(config, *state->input, def.id);
            if (listening) screen.listening.clear();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SetCursorPosX(left);
    ImGui::TextDisabled("Click a key, then press the new one. Esc cancels; Backspace or Delete unbinds.");

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (GUIController::DrawMenuButton("Reset All"))
    {
        KeyBindings::ResetAll(config, *state->input);
        screen.listening.clear();
    }
    if (GUIController::DrawMenuButton("Done"))
    {
        screen.listening.clear();
        if (onDone) onDone();
    }
}

} // namespace

Screen* CreateKeyBindsScreen(GlobalState* state, std::function<void()> onDone)
{
    Screen* screen = GUIController::CreateScreen("Key Binds", /* closable */ false);

    auto screenState = std::make_shared<ScreenState>();
    screen->customContent = [state, screen, screenState, onDone]
    {
        // Drawn here since customContent replaces the generic title.
        ImGui::GetFont()->Scale = 1.15f;
        ImVec2 titleSize = ImGui::CalcTextSize(screen->title.c_str());
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleSize.x) * 0.5f);
        ImGui::Text("%s", screen->title.c_str());
        ImGui::GetFont()->Scale = 1.00f;
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        DrawKeyBinds(state, *screenState, onDone);
    };
    return screen;
}

} // namespace Volcano::GUI
