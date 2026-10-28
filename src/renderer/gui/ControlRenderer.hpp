#pragma once
#ifndef VOLCANO_GUI_CONTROL_RENDERER_H
#define VOLCANO_GUI_CONTROL_RENDERER_H

#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <Pincel/Pincel.hpp>
#include "ImGuiInput.hpp"
#include "PincelSurface.hpp"
#include "models/Screen.hpp"

namespace Volcano {
struct GlobalState;
}

namespace Volcano::GUI {

// Draws a Screen's components (text, buttons, inputs, sliders, number fields
// and options) with Pincel instead of ImGui. Each component becomes a few
// Pincel elements, kept in step with it every frame; Pincel only reports what
// the mouse did, and the component's own handlers decide what that means.
// Render thread only.
class ControlRenderer {
public:
    explicit ControlRenderer(GlobalState* globalState) : state(globalState) {}

    // The Minecraft font with its fallbacks, loaded on first use. Shared by everything Pincel draws.
    static Pincel::FontFamily UiFont();

    bool Init(VkRenderPass renderPass);
    void Shutdown();

    // Screens with customContent draw themselves with raw ImGui, so they aren't this renderer's.
    static bool Draws(const Screen& screen) { return !screen.customContent; }

    // For a Screen's build and update callbacks: the standard look, for elements of their own.
    // The mouse as last fed to Pincel, in pixels.
    const Pincel::MouseInput& Mouse() const { return input.Mouse(); }
    // Where the screen's rows go.
    Pincel::Element Column() const { return column; }
    // The canvas's root, for elements placed over the whole screen (Fixed), and the dimmed backdrop behind everything.
    Pincel::Element Overlay() const { return canvas->Root(); }
    Pincel::Element Backdrop() const { return wrapper; }
    // The canvas size in pixels.
    int Width() const { return static_cast<int>(width); }
    int Height() const { return static_cast<int>(height); }
    // Pixels for a size in UI units (GUI scale applied).
    int Units(float units) const;
    // A button `width` units wide. Hover and press are drawn for you; check WasClicked() on it.
    Pincel::Element AddButton(Pincel::Element parent, const std::string& label, float width);
    // A single-line text field `width` units wide; returns the Input, which you read and set.
    Pincel::Element AddField(Pincel::Element parent, float width, const std::string& hint = "");
    void PlayClick();
    // Builds the screen again next frame, for when what it shows changed in number.
    void Rebuild() { builtScreen = nullptr; }

    // Brings the screen up to date, gives Pincel this frame's input and draws it
    // to the GPU texture. `scale` is the GUI scale (pixels per UI unit). Call it
    // after GUIController::NewFrame and outside the render pass.
    void Update(Screen& screen, float scale);

    // Lays the finished screen over the frame, inside the main render pass.
    void Record(VkCommandBuffer commandBuffer);

    // Forgets the built screen so its images are read again (after a resource reload).
    void ReloadResources();

private:
    // What a component turned into. Not every element is used by every kind.
    struct Control {
        GUIComponent* component = nullptr;
        Pincel::Element row;       // Everything of it that is hidden with it.
        Pincel::Element labelCell; // The label beside a labeled control.
        Pincel::Element body;      // What the mouse hits: the button, track, or field.
        Pincel::Element text;
        Pincel::Element grab;      // A slider's handle.
        Pincel::Element input;
        int column = 0;            // Which column of a multi-column grid it is in.
        bool wasFocused = false;
    };

    struct Field {
        Pincel::Element body;
        Pincel::Element input;
    };
    Pincel::Element MakeButton(Pincel::Element parent, const std::string& label, float width, Pincel::Element& text);
    Field MakeField(Pincel::Element parent, float width);
    void StyleButton(Pincel::Element body);
    void StyleField(Field field);
    void AddLogo(Pincel::Element parent);

    void Build(Screen& screen);
    void BuildRun(Pincel::Element column, Screen& screen, size_t first, size_t last);
    void BuildSingle(Pincel::Element column, GUIComponent* component);
    void BuildLabeled(Pincel::Element labels, Pincel::Element controlsColumn, GUIComponent* component, int columnIndex);

    void FeedInput();
    void Refresh(Screen& screen);
    void React(Screen& screen);
    void ReactButton(Control& control);
    void ReactInput(Control& control);
    void ReactNumber(Control& control);
    void ReactSlider(Control& control);
    void ReactDropdown(Control& control, size_t index);
    void ReactPopup();
    void UpdateTooltip(const Screen& screen);

    void OpenDropdown(size_t controlIndex);
    void CloseDropdown();

    GlobalState* state;
    PincelSurface surface;
    std::unique_ptr<Pincel::Canvas> canvas;
    bool ready = false;

    const Screen* builtScreen = nullptr;
    std::vector<GUIComponent*> builtComponents;
    int scale = 1;
    uint32_t width = 0;
    uint32_t height = 0;

    Pincel::Element wrapper;
    Pincel::Element column;
    Pincel::Element title;
    std::vector<Pincel::Element> extraButtons; // From AddButton, restyled each frame.
    std::vector<Field> extraFields;            // From AddField.
    std::vector<Control> controls;
    ImGuiInput input;

    Pincel::Element tooltip;
    Pincel::Element tooltipText;

    int openDropdown = -1;
    Pincel::Element popupBackdrop;
    Pincel::Element popupList;
    std::vector<Pincel::Element> popupRows;
};

} // namespace Volcano::GUI

#endif
