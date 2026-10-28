#include "ImGuiInput.hpp"
#include <imgui.h>

namespace Volcano::GUI {

namespace {

Pincel::Key ToPincelKey(ImGuiKey key)
{
    switch (key)
    {
        case ImGuiKey_Backspace: return Pincel::Key::Backspace;
        case ImGuiKey_Delete: return Pincel::Key::Delete;
        case ImGuiKey_Enter:
        case ImGuiKey_KeypadEnter: return Pincel::Key::Enter;
        case ImGuiKey_Tab: return Pincel::Key::Tab;
        case ImGuiKey_Escape: return Pincel::Key::Escape;
        case ImGuiKey_LeftArrow: return Pincel::Key::Left;
        case ImGuiKey_RightArrow: return Pincel::Key::Right;
        case ImGuiKey_UpArrow: return Pincel::Key::Up;
        case ImGuiKey_DownArrow: return Pincel::Key::Down;
        case ImGuiKey_Home: return Pincel::Key::Home;
        case ImGuiKey_End: return Pincel::Key::End;
        case ImGuiKey_PageUp: return Pincel::Key::PageUp;
        case ImGuiKey_PageDown: return Pincel::Key::PageDown;
        default: break;
    }
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
    {
        return static_cast<Pincel::Key>(static_cast<int>(Pincel::Key::A) + (key - ImGuiKey_A));
    }
    return Pincel::Key::Unknown;
}

// Every key Pincel's inputs react to, for polling ImGui's key state.
constexpr ImGuiKey WATCHED_KEYS[] = {
    ImGuiKey_Backspace, ImGuiKey_Delete, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Tab, ImGuiKey_Escape,
    ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
    ImGuiKey_Home, ImGuiKey_End, ImGuiKey_PageUp, ImGuiKey_PageDown,
    ImGuiKey_A, ImGuiKey_B, ImGuiKey_C, ImGuiKey_D, ImGuiKey_E, ImGuiKey_F, ImGuiKey_G, ImGuiKey_H, ImGuiKey_I,
    ImGuiKey_J, ImGuiKey_K, ImGuiKey_L, ImGuiKey_M, ImGuiKey_N, ImGuiKey_O, ImGuiKey_P, ImGuiKey_Q, ImGuiKey_R,
    ImGuiKey_S, ImGuiKey_T, ImGuiKey_U, ImGuiKey_V, ImGuiKey_W, ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z,
};

} // namespace

void ImGuiInput::Feed(Pincel::Canvas& canvas, float scale)
{
    ImGuiIO& io = ImGui::GetIO();

    mouse = Pincel::MouseInput{};
    if (ImGui::IsMousePosValid(&io.MousePos))
    {
        mouse.x = io.MousePos.x * scale;
        mouse.y = io.MousePos.y * scale;
    }
    if (io.MouseDown[0]) mouse.buttons |= Pincel::MouseButton::Left;
    if (io.MouseDown[1]) mouse.buttons |= Pincel::MouseButton::Right;
    if (io.MouseDown[2]) mouse.buttons |= Pincel::MouseButton::Middle;
    mouse.scrollX = io.MouseWheelH;
    mouse.scrollY = io.MouseWheel;

    Pincel::KeyboardInput keyboard;
    for (ImGuiKey key : WATCHED_KEYS)
    {
        Pincel::Key mapped = ToPincelKey(key);
        if (ImGui::IsKeyPressed(key, false)) keyboard.events.push_back({ mapped, Pincel::KeyAction::Press });
        else if (ImGui::IsKeyPressed(key, true)) keyboard.events.push_back({ mapped, Pincel::KeyAction::Repeat });
        if (ImGui::IsKeyReleased(key)) keyboard.events.push_back({ mapped, Pincel::KeyAction::Release });
    }
    for (int i = 0; i < io.InputQueueCharacters.Size; i++)
    {
        char32_t character = io.InputQueueCharacters[i];
        // A character outside the first plane arrives as a surrogate pair when ImWchar is 16 bits.
        if (character >= 0xD800 && character < 0xDC00 && i + 1 < io.InputQueueCharacters.Size)
        {
            char32_t low = io.InputQueueCharacters[i + 1];
            if (low >= 0xDC00 && low < 0xE000)
            {
                character = 0x10000 + ((character - 0xD800) << 10) + (low - 0xDC00);
                i++;
            }
        }
        if (character >= 32 && character != 127) keyboard.text.push_back(character);
    }
    if (io.KeyShift) keyboard.modifiers |= Pincel::Modifier::Shift;
    if (io.KeyCtrl) keyboard.modifiers |= Pincel::Modifier::Control;
    if (io.KeyAlt) keyboard.modifiers |= Pincel::Modifier::Alt;
    if (io.KeySuper) keyboard.modifiers |= Pincel::Modifier::Super;

    canvas.SetInput(mouse, keyboard);
}

} // namespace Volcano::GUI
