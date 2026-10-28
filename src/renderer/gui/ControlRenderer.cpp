#include "ControlRenderer.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <optional>
#include <imgui.h>
#include "GlobalState.hpp"
#include "Logger.hpp"
#include "audio/SoundRegistry.hpp"
#include "platform/OS.hpp"
#include "../VulkanInit.hpp"
#include "PincelSprite.hpp"
#include "Theme.hpp"
#include "models/NumberInput.hpp"
#include "models/OptionButton.hpp"
#include "models/Slider.hpp"

namespace Volcano::GUI {

namespace {

using Pincel::Align;
using Pincel::Color;
using Pincel::Element;
using Pincel::Orientation;
using Pincel::PointerEvents;
using Pincel::Position;

using namespace Theme;

// Sizes are in UI units, which Units() multiplies by the GUI scale.
// Minecraft.ttf is an 8 pixel font, so sizes that are multiples of 8 stay crisp.
constexpr float FONT_SIZE = 16.0f;
constexpr float TITLE_SIZE = 24.0f;
constexpr float BUTTON_WIDTH = 280.0f;
constexpr float CONTROL_WIDTH = 160.0f;
constexpr float CONTROL_HEIGHT = 40.0f; // Sliders, fields and dropdowns.
constexpr float BUTTON_PADDING_Y = 8.0f; // Above and below a button's text.
constexpr float BUTTON_PADDING_X = 12.0f; // Left and right of it.
constexpr float ITEM_GAP = 4.0f;
constexpr float COLUMN_GAP = 16.0f;
constexpr float LABEL_GAP = 12.0f;
constexpr float TITLE_GAP = 12.0f;
constexpr float SCREEN_MARGIN = 24.0f;
constexpr float BUTTON_RADIUS = 12.0f;
constexpr float FRAME_RADIUS = 12.0f;
constexpr float BUTTON_BORDER_SIZE = 1.0f;
constexpr float FRAME_BORDER_SIZE = 2.0f;
constexpr float FIELD_PADDING = 12.0f;
constexpr float GRAB_WIDTH = 6.0f;
constexpr float LOGO_SIZE = 128.0f;
constexpr float TOOLTIP_WIDTH = 220.0f;
constexpr float TOOLTIP_PADDING = 8.0f;
constexpr float TOOLTIP_GAP = 12.0f;
constexpr float OPTION_HEIGHT = 26.0f;
constexpr size_t OPTIONS_SHOWN = 8;
constexpr float FADE_SECONDS = 0.1f;

constexpr const char* CLICK_SOUND = "minecraft:random/click_stereo";
constexpr const char* LOGO_PATH = "resources/logo.png";
constexpr const char* FONT_PATH = "resources/fonts/Minecraft.ttf";
constexpr const char* FONT_NAME = "Minecraft";

constexpr std::nullopt_t OMIT = std::nullopt;

// Insets from the sides given (OMIT leaves one out).
Pincel::Insets MakeInsets(std::optional<int> left, std::optional<int> top, std::optional<int> right,
                          std::optional<int> bottom, bool centerX = false, bool centerY = false)
{
    Pincel::Insets insets;
    insets.left = left;
    insets.top = top;
    insets.right = right;
    insets.bottom = bottom;
    insets.centerX = centerX;
    insets.centerY = centerY;
    return insets;
}

bool IsLabeled(GUIComponent* component)
{
    GUIComponentType type = component->GetType();
    return type == GUIComponentType::SLIDER || type == GUIComponentType::NUMBER || type == GUIComponentType::OPTION;
}

// Loads the UI font, with the system fonts as fallbacks for characters it lacks.
// Families are shared by everything using Pincel and can't be removed, so a second
// call (after a restart of the GUI) reuses what is there.
Pincel::FontFamily LoadUiFont()
{
    if (auto existing = Pincel::FindFontFamily(FONT_NAME)) return *existing;

    auto family = Pincel::LoadFontFamilyFiles(FONT_NAME, FONT_PATH);
    if (!family)
    {
        Log::Error(std::string("ControlRenderer: could not load ") + FONT_PATH + ": " + std::string(Pincel::LastError()));
        return Pincel::FontFamily::Sans;
    }

    // Pincel looks through a few families deep.
    Pincel::FontFamily last = *family;
    int index = 0;
    for (const std::string& path : Platform::OS::GetFallbackFontPaths())
    {
        if (index == 3) break;
        auto fallback = Pincel::LoadFontFamilyFiles(std::string(FONT_NAME) + " Fallback " + std::to_string(index), path);
        if (!fallback) continue;
        Pincel::SetFontFallback(last, *fallback);
        last = *fallback;
        index++;
    }
    return *family;
}

} // namespace

Pincel::FontFamily ControlRenderer::UiFont()
{
    return LoadUiFont();
}

int ControlRenderer::Units(float units) const
{
    return std::max(0, static_cast<int>(std::lround(units * static_cast<float>(scale))));
}

bool ControlRenderer::Init(VkRenderPass renderPass)
{
    Pincel::Init();
    if (!surface.Create(renderPass)) return false;

    canvas = std::make_unique<Pincel::Canvas>();
    canvas->SetBackground(Color{ 0, 0, 0, 0.0f });
    canvas->SetForeground(LABEL_TEXT);
    canvas->SetClipboard(
        [] { const char* text = ImGui::GetClipboardText(); return std::string(text != nullptr ? text : ""); },
        [](const std::string& text) { ImGui::SetClipboardText(text.c_str()); });

    // The default text style for everything under the canvas.
    Element root = canvas->Root();
    root.SetFontFamily(LoadUiFont());
    root.SetFontSize(static_cast<std::uint16_t>(FONT_SIZE));

    ready = true;
    return true;
}

void ControlRenderer::Shutdown()
{
    ready = false;
    controls.clear();
    popupRows.clear();
    canvas.reset();
    surface.Destroy();
    builtScreen = nullptr;
    builtComponents.clear();
}

void ControlRenderer::ReloadResources()
{
    builtScreen = nullptr;
}

void ControlRenderer::PlayClick()
{
    if (state->soundRegistry != nullptr) state->soundRegistry->Play(state->audioManager, CLICK_SOUND);
}

// ---- Building ----

void ControlRenderer::Build(Screen& screen)
{
    builtScreen = &screen;
    builtComponents = screen.components;
    controls.clear();
    popupRows.clear();
    extraButtons.clear();
    extraFields.clear();
    openDropdown = -1;

    Element root = canvas->Root();
    root.RemoveChildren();
    int fontPixels = Units(FONT_SIZE);
    if (screen.fontSize > 0.0f) fontPixels = static_cast<int>(std::lround(screen.fontSize * static_cast<float>(screen.fontInArtPixels ? scale + 1 : scale)));
    root.SetFontSize(static_cast<std::uint16_t>(fontPixels));
    int shadow = screen.fontInArtPixels ? scale + 1 : Units(2.0f);
    canvas->SetTextShadow(SHADOW, shadow, shadow);

    // The darkened backdrop, as tall as the screen: what doesn't fit scrolls.
    wrapper = root.AddStack(Orientation::Vertical);
    wrapper.SetSize(static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height))
        .SetBackground(SCREEN_DIM)
        .SetOverflow(Pincel::Overflow::Scroll)
        .SetAlign(Align::Center, Align::Center)
        .SetPadding(static_cast<std::uint16_t>(Units(SCREEN_MARGIN)), 0, static_cast<std::uint16_t>(Units(SCREEN_MARGIN)), 0);

    column = wrapper.AddStack(Orientation::Vertical);
    column.SetAlign(Align::Center, Align::Start).SetGap(static_cast<std::uint16_t>(Units(ITEM_GAP)));

    title = Element();
    if (screen.showTitle)
    {
        if (screen.showLogo) AddLogo(column);
        if (!screen.showLogo || column.ChildCount() == 0)
        {
            title = column.AddText(screen.title);
            title.SetFontSize(static_cast<std::uint16_t>(Units(TITLE_SIZE)));
        }
        column.AddStack(Orientation::Vertical).SetSize(1, static_cast<std::uint16_t>(Units(TITLE_GAP)));
    }

    // Runs of labeled controls share a grid; everything else is its own row.
    size_t i = 0;
    while (i < screen.components.size())
    {
        if (IsLabeled(screen.components[i]))
        {
            size_t end = i;
            while (end < screen.components.size() && IsLabeled(screen.components[end])) end++;
            BuildRun(column, screen, i, end);
            i = end;
        }
        else
        {
            BuildSingle(column, screen.components[i]);
            i++;
        }
    }
    if (screen.build) screen.build(*this);

    // Over everything else, and never in the mouse's way.
    tooltip = root.AddStack(Orientation::Vertical);
    tooltip.SetPosition(Position::Fixed)
        .SetPointerEvents(PointerEvents::None)
        .SetSize(static_cast<std::uint16_t>(Units(TOOLTIP_WIDTH)), 0)
        .SetPadding(static_cast<std::uint16_t>(Units(TOOLTIP_PADDING)))
        .SetBackground(PANEL_DARK)
        .SetBorder(static_cast<std::uint16_t>(Units(FRAME_BORDER_SIZE)), PANEL_LIGHT)
        .SetCornerRadius(static_cast<std::uint16_t>(Units(FRAME_RADIUS)))
        .SetVisible(false);
    tooltipText = tooltip.AddText("");
}

// The logo is a big image that Pincel would shrink every time it drew, so it is
// shrunk once here, to the size it is shown at, and drawn as a plain copy.
void ControlRenderer::AddLogo(Element parent)
{
    int target = std::max(1, Units(LOGO_SIZE));
    Sprite::Load(LOGO_PATH).Resized(target, target).AddTo(parent);
}

Element ControlRenderer::MakeButton(Element parent, const std::string& label, float buttonWidth, Element& text)
{
    Element body = parent.AddStack(Orientation::Vertical);
    // The height is the text's plus the padding, so bigger text or padding makes a bigger button.
    body.SetSize(static_cast<uint16_t>(Units(buttonWidth)), 0)
        .SetPadding(static_cast<uint16_t>(Units(BUTTON_PADDING_Y)), static_cast<uint16_t>(Units(BUTTON_PADDING_X)),
            static_cast<uint16_t>(Units(BUTTON_PADDING_Y)), static_cast<uint16_t>(Units(BUTTON_PADDING_X)))
        .SetAlign(Align::Center, Align::Center)
        .SetBackground(BUTTON_FILL)
        .SetBorder(static_cast<uint16_t>(Units(BUTTON_BORDER_SIZE)), BUTTON_BORDER)
        .SetCornerRadius(static_cast<uint16_t>(Units(BUTTON_RADIUS)))
        .SetTransition(FADE_SECONDS);
    text = body.AddText(label);
    text.SetForeground(BUTTON_TEXT);
    return body;
}

ControlRenderer::Field ControlRenderer::MakeField(Element parent, float fieldWidth)
{
    uint16_t border = static_cast<uint16_t>(Units(FRAME_BORDER_SIZE));
    uint16_t padding = static_cast<uint16_t>(Units(FIELD_PADDING));

    Field field;
    field.body = parent.AddStack(Orientation::Horizontal);
    field.body.SetSize(static_cast<uint16_t>(Units(fieldWidth)), static_cast<uint16_t>(Units(CONTROL_HEIGHT)))
        .SetAlign(Align::Start, Align::Center)
        .SetPadding(0, padding, 0, padding)
        .SetBackground(PANEL_MID)
        .SetBorder(border, PANEL_LIGHT)
        .SetCornerRadius(static_cast<uint16_t>(Units(FRAME_RADIUS)))
        .SetTransition(FADE_SECONDS);
    field.input = field.body.AddInput(Pincel::InputKind::Text);
    field.input.SetSize(static_cast<uint16_t>(std::max(1, Units(fieldWidth) - 2 * (border + padding))), 0);
    return field;
}

Element ControlRenderer::AddButton(Element parent, const std::string& label, float buttonWidth)
{
    Element text;
    Element body = MakeButton(parent, label, buttonWidth, text);
    extraButtons.push_back(body);
    return body;
}

Element ControlRenderer::AddField(Element parent, float fieldWidth, const std::string& hint)
{
    Field field = MakeField(parent, fieldWidth);
    field.input.SetPlaceholder(hint);
    extraFields.push_back(field);
    return field.input;
}

void ControlRenderer::StyleButton(Element body)
{
    bool pressed = body.IsPressed(Pincel::MouseButton::Left);
    bool hovered = body.IsHovered();
    body.SetBackground(pressed ? ORANGE_DIM : BUTTON_FILL);
    body.SetBorder(static_cast<uint16_t>(Units(BUTTON_BORDER_SIZE)), hovered || pressed ? ORANGE_DIM : BUTTON_BORDER);
}

void ControlRenderer::StyleField(Field field)
{
    if (field.body.WasClicked(Pincel::MouseButton::Left) && !field.input.IsFocused()) field.input.Focus();
    bool focused = field.input.IsFocused();
    field.body.SetBackground(focused || field.body.IsHovered() ? PANEL_LIGHT : PANEL_MID);
    field.body.SetBorder(static_cast<uint16_t>(Units(FRAME_BORDER_SIZE)), focused ? ORANGE : PANEL_LIGHT);
}

// A run of labeled controls as columns of labels beside columns of controls, so
// each column's labels take the width of its longest and the controls line up.
void ControlRenderer::BuildRun(Element column, Screen& screen, size_t first, size_t last)
{
    int columns = std::clamp(screen.columns, 1, static_cast<int>(last - first));
    uint16_t gap = static_cast<uint16_t>(Units(ITEM_GAP));

    Element grid = column.AddStack(Orientation::Horizontal);
    grid.SetGap(static_cast<uint16_t>(Units(COLUMN_GAP)));

    std::vector<Element> labelColumns;
    std::vector<Element> controlColumns;
    for (int c = 0; c < columns; c++)
    {
        Element group = grid.AddStack(Orientation::Horizontal);
        group.SetGap(static_cast<uint16_t>(Units(LABEL_GAP)));
        labelColumns.push_back(group.AddStack(Orientation::Vertical).SetGap(gap));
        controlColumns.push_back(group.AddStack(Orientation::Vertical).SetGap(gap));
    }

    for (size_t k = first; k < last; k++)
    {
        int c = static_cast<int>((k - first) % static_cast<size_t>(columns));
        BuildLabeled(labelColumns[c], controlColumns[c], screen.components[k], c);
    }
}

void ControlRenderer::BuildSingle(Element column, GUIComponent* component)
{
    Control control;
    control.component = component;

    switch (component->GetType())
    {
        case GUIComponentType::BUTTON:
        {
            control.row = control.body = MakeButton(column, component->GetLabel(), BUTTON_WIDTH, control.text);
            break;
        }
        case GUIComponentType::INPUT:
        {
            if (component->GetTextBuffer() == nullptr) return;
            control.row = column.AddStack(Orientation::Vertical);
            control.row.SetSize(static_cast<uint16_t>(Units(BUTTON_WIDTH)), 0).SetGap(static_cast<uint16_t>(Units(2.0f)));
            control.text = control.row.AddText(component->GetLabel());
            control.text.SetForeground(TEXT_DIM);

            Field field = MakeField(control.row, BUTTON_WIDTH);
            control.body = field.body;
            control.input = field.input;
            break;
        }
        case GUIComponentType::TEXT:
        default:
        {
            control.row = control.text = column.AddText(component->GetLabel());
            break;
        }
    }
    controls.push_back(control);
}

void ControlRenderer::BuildLabeled(Element labels, Element controlsColumn, GUIComponent* component, int columnIndex)
{
    Control control;
    control.component = component;
    control.column = columnIndex;

    uint16_t height = static_cast<uint16_t>(Units(CONTROL_HEIGHT));
    uint16_t border = static_cast<uint16_t>(Units(FRAME_BORDER_SIZE));
    uint16_t padding = static_cast<uint16_t>(Units(FIELD_PADDING));

    control.labelCell = labels.AddStack(Orientation::Horizontal);
    control.labelCell.SetSize(0, height).SetAlign(Align::Start, Align::Center);
    control.labelCell.AddText(component->GetLabel());

    control.row = control.body = controlsColumn.AddStack(Orientation::Vertical);
    control.body.SetSize(static_cast<uint16_t>(Units(CONTROL_WIDTH)), height)
        .SetBackground(PANEL_MID)
        .SetBorder(border, PANEL_LIGHT)
        .SetCornerRadius(static_cast<uint16_t>(Units(FRAME_RADIUS)))
        .SetTransition(FADE_SECONDS);

    switch (component->GetType())
    {
        case GUIComponentType::SLIDER:
        {
            control.grab = control.body.AddStack(Orientation::Vertical);
            control.grab.SetPosition(Position::Absolute)
                .SetInsets(MakeInsets(0, 0, OMIT, 0))
                .SetSize(static_cast<uint16_t>(std::max(1, Units(GRAB_WIDTH))), 0)
                .SetBackground(ORANGE)
                .SetPointerEvents(PointerEvents::None);
            control.text = control.body.AddText("");
            control.text.SetPosition(Position::Absolute)
                .SetInsets(MakeInsets(OMIT, OMIT, OMIT, OMIT, true, true))
                .SetPointerEvents(PointerEvents::None);
            break;
        }
        case GUIComponentType::NUMBER:
        {
            control.body.SetAlign(Align::Start, Align::Center).SetPadding(0, padding, 0, padding);
            control.input = control.body.AddInput(Pincel::InputKind::Number);
            control.input.SetSize(static_cast<uint16_t>(Units(CONTROL_WIDTH) - 2 * (border + padding)), 0);
            break;
        }
        case GUIComponentType::OPTION:
        default:
        {
            control.text = control.body.AddText("");
            control.text.SetPosition(Position::Absolute)
                .SetInsets(MakeInsets(padding, OMIT, OMIT, OMIT, false, true))
                .SetPointerEvents(PointerEvents::None);
            Element arrow = control.body.AddText("v");
            arrow.SetPosition(Position::Absolute)
                .SetInsets(MakeInsets(OMIT, OMIT, padding, OMIT, false, true))
                .SetForeground(TEXT_DIM)
                .SetPointerEvents(PointerEvents::None);
            break;
        }
    }
    controls.push_back(control);
}

// ---- Per frame ----

void ControlRenderer::FeedInput()
{
    input.Feed(*canvas, static_cast<float>(scale));
}

// What the components say now: labels, visibility and values. Safe to call
// every frame, since setting what is already there costs nothing.
void ControlRenderer::Refresh(Screen& screen)
{
    if (title.IsValid()) title.SetText(screen.title);

    for (Control& control : controls)
    {
        GUIComponent* component = control.component;
        bool visible = component->IsVisible();
        control.row.SetVisible(visible);
        if (control.labelCell.IsValid()) control.labelCell.SetVisible(visible);

        switch (component->GetType())
        {
            case GUIComponentType::TEXT:
            case GUIComponentType::BUTTON:
                control.text.SetText(component->GetLabel());
                break;
            case GUIComponentType::INPUT:
                control.text.SetText(component->GetLabel());
                if (!control.input.IsFocused()) control.input.SetText(component->GetTextBuffer());
                break;
            case GUIComponentType::NUMBER:
                if (!control.input.IsFocused())
                {
                    control.input.SetText(std::to_string(static_cast<NumberInput*>(component)->GetValue()));
                }
                break;
            case GUIComponentType::OPTION:
                control.text.SetText(static_cast<OptionButton*>(component)->GetValueText());
                break;
            default:
                break;
        }
    }
}

void ControlRenderer::Update(Screen& screen, float guiScale)
{
    if (!ready || swapchainExtent.width == 0 || swapchainExtent.height == 0) return;

    width = swapchainExtent.width;
    height = swapchainExtent.height;
    canvas->SetSize(width, height);

    int newScale = std::max(1, static_cast<int>(std::lround(guiScale)));
    if (builtScreen != &screen || newScale != scale || builtComponents != screen.components)
    {
        scale = newScale;
        Build(screen);
    }
    wrapper.SetSize(static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height));

    Refresh(screen);
    FeedInput();
    Pincel::Texture* texture = canvas->Draw();
    React(screen);
    surface.Upload(*texture);
}

void ControlRenderer::Record(VkCommandBuffer commandBuffer)
{
    if (ready) surface.Record(commandBuffer);
}

// ---- Reacting to the mouse ----

void ControlRenderer::React(Screen& screen)
{
    // A popup, if open, is all the mouse can reach.
    ReactPopup();

    // A click can open another screen, which is built on the next Update; this one's controls stay valid until then.
    for (size_t i = 0; i < controls.size(); i++)
    {
        if (openDropdown >= 0) break;
        Control& control = controls[i];
        if (!control.component->IsVisible()) continue;

        switch (control.component->GetType())
        {
            case GUIComponentType::BUTTON: ReactButton(control); break;
            case GUIComponentType::INPUT: ReactInput(control); break;
            case GUIComponentType::NUMBER: ReactNumber(control); break;
            case GUIComponentType::SLIDER: ReactSlider(control); break;
            case GUIComponentType::OPTION: ReactDropdown(control, i); break;
            default: break;
        }
    }
    UpdateTooltip(screen);

    for (Element& button : extraButtons) StyleButton(button);
    for (const Field& field : extraFields) StyleField(field);
    if (screen.update) screen.update(*this);
}

void ControlRenderer::ReactButton(Control& control)
{
    StyleButton(control.body);

    if (control.body.WasClicked(Pincel::MouseButton::Left))
    {
        PlayClick();
        control.component->Click();
    }
}

void ControlRenderer::ReactInput(Control& control)
{
    // What was typed goes back into the component's buffer.
    if (control.input.InputChanged())
    {
        std::string text = control.input.GetText();
        char* buffer = control.component->GetTextBuffer();
        size_t capacity = control.component->GetTextBufferCapacity();
        if (buffer != nullptr && capacity > 0)
        {
            size_t length = std::min(text.size(), capacity - 1);
            std::copy_n(text.begin(), length, buffer);
            buffer[length] = '\0';
        }
    }

    StyleField({ control.body, control.input });
}

void ControlRenderer::ReactNumber(Control& control)
{
    auto* number = static_cast<NumberInput*>(control.component);
    if (control.body.WasClicked(Pincel::MouseButton::Left) && !control.input.IsFocused()) control.input.Focus();

    // Committed when editing finishes (Enter or leaving the field), not on every keystroke.
    bool focused = control.input.IsFocused();
    bool submitted = control.input.InputSubmitted();
    if (submitted || (control.wasFocused && !focused))
    {
        try
        {
            number->SetValue(std::clamp(std::stoi(control.input.GetText()), number->GetMin(), number->GetMax()));
        }
        catch (const std::exception&)
        {
            // Nothing usable typed ("" or "-"): keep the value, which the field shows again.
        }
        if (submitted)
        {
            control.input.Blur();
            focused = false;
        }
    }
    control.wasFocused = focused;

    control.body.SetBackground(focused || control.body.IsHovered() ? PANEL_LIGHT : PANEL_MID);
    control.body.SetBorder(static_cast<uint16_t>(Units(FRAME_BORDER_SIZE)), focused ? ORANGE : PANEL_LIGHT);
}

void ControlRenderer::ReactSlider(Control& control)
{
    auto* slider = static_cast<Slider*>(control.component);
    Element::Rect rect = control.body.GetRect();
    int border = Units(FRAME_BORDER_SIZE);
    int grabWidth = std::max(1, Units(GRAB_WIDTH));
    int track = rect.w - 2 * border - grabWidth;
    int32_t span = slider->GetMax() - slider->GetMin();

    // Dragging only previews the value; letting go commits it, so a setting that rebuilds the swapchain doesn't do so every frame.
    bool pressed = control.body.IsPressed(Pincel::MouseButton::Left);
    if (pressed && track > 0 && !std::isnan(input.Mouse().x))
    {
        float t = (input.Mouse().x - static_cast<float>(rect.x + border + grabWidth / 2)) / static_cast<float>(track);
        slider->editing = true;
        slider->editValue = slider->GetMin() + static_cast<int32_t>(std::lround(std::clamp(t, 0.0f, 1.0f) * static_cast<float>(span)));
    }
    else if (slider->editing)
    {
        slider->editing = false;
        PlayClick();
        slider->SetValue(slider->editValue);
    }
    if (!slider->editing) slider->editValue = slider->GetValue();

    float fraction = span > 0 ? static_cast<float>(slider->editValue - slider->GetMin()) / static_cast<float>(span) : 0.0f;
    control.grab.SetInsets(MakeInsets(
        static_cast<int>(std::lround(fraction * static_cast<float>(std::max(0, track)))), 0, OMIT, 0));
    control.text.SetText(std::to_string(slider->editValue) + slider->GetSuffix());

    control.body.SetBackground(control.body.IsHovered() || pressed ? PANEL_LIGHT : PANEL_MID);
}

void ControlRenderer::ReactDropdown(Control& control, size_t index)
{
    control.body.SetBackground(control.body.IsHovered() ? PANEL_LIGHT : PANEL_MID);
    if (control.body.WasClicked(Pincel::MouseButton::Left)) OpenDropdown(index);
}

// ---- Dropdown list ----

void ControlRenderer::OpenDropdown(size_t controlIndex)
{
    auto* option = static_cast<OptionButton*>(controls[controlIndex].component);
    const std::vector<std::string>& options = option->GetOptions();
    if (options.empty()) return;

    Element::Rect anchor = controls[controlIndex].body.GetRect();
    int rowHeight = Units(OPTION_HEIGHT);
    int padding = Units(FRAME_BORDER_SIZE);
    int shown = static_cast<int>(std::min(options.size(), OPTIONS_SHOWN));
    int listHeight = shown * rowHeight + 2 * padding;

    // Below the control, or above it when there is no room.
    int top = anchor.y + anchor.h + Units(2.0f);
    if (top + listHeight > static_cast<int>(height)) top = std::max(0, anchor.y - Units(2.0f) - listHeight);

    Element root = canvas->Root();

    // A click anywhere else closes it, and doesn't reach what is behind.
    popupBackdrop = root.AddStack(Orientation::Vertical);
    popupBackdrop.SetPosition(Position::Fixed).SetInsets(MakeInsets(0, 0, 0, 0));

    popupList = root.AddStack(Orientation::Vertical);
    popupList.SetPosition(Position::Fixed)
        .SetInsets(MakeInsets(anchor.x, top, OMIT, OMIT))
        .SetSize(static_cast<uint16_t>(anchor.w), static_cast<uint16_t>(listHeight))
        .SetPadding(static_cast<uint16_t>(padding))
        .SetBackground(PANEL_DARK)
        .SetBorder(static_cast<uint16_t>(Units(FRAME_BORDER_SIZE)), PANEL_LIGHT)
        .SetCornerRadius(static_cast<uint16_t>(Units(FRAME_RADIUS)))
        .SetOverflow(Pincel::Overflow::Scroll);

    size_t current = option->GetIndex();
    int rowWidth = anchor.w - 4 * padding;
    popupRows.clear();
    for (size_t i = 0; i < options.size(); i++)
    {
        Element row = popupList.AddStack(Orientation::Horizontal);
        row.SetSize(static_cast<uint16_t>(rowWidth), static_cast<uint16_t>(rowHeight))
            .SetAlign(Align::Start, Align::Center)
            .SetPadding(0, static_cast<uint16_t>(Units(FIELD_PADDING)), 0, static_cast<uint16_t>(Units(FIELD_PADDING)))
            .SetCornerRadius(static_cast<uint16_t>(Units(BUTTON_RADIUS)));
        row.AddText(options[i]).SetForeground(i == current ? ORANGE : LABEL_TEXT);
        popupRows.push_back(row);
    }
    openDropdown = static_cast<int>(controlIndex);
}

void ControlRenderer::CloseDropdown()
{
    popupBackdrop.Remove();
    popupList.Remove();
    popupRows.clear();
    openDropdown = -1;
}

void ControlRenderer::ReactPopup()
{
    if (openDropdown < 0) return;
    auto* option = static_cast<OptionButton*>(controls[static_cast<size_t>(openDropdown)].component);

    for (size_t i = 0; i < popupRows.size(); i++)
    {
        popupRows[i].SetBackground(popupRows[i].IsHovered() ? PANEL_LIGHT : Color{ 0, 0, 0, 0.0f });
        if (popupRows[i].WasClicked(Pincel::MouseButton::Left))
        {
            option->Select(i);
            CloseDropdown();
            return;
        }
    }
    if (popupBackdrop.WasClicked(Pincel::MouseButton::Left)) CloseDropdown();
}

// ---- Tooltip ----

// Beside the hovered control rather than under the cursor, so it never covers its neighbors.
void ControlRenderer::UpdateTooltip(const Screen& screen)
{
    const Control* hovered = nullptr;
    if (openDropdown < 0)
    {
        for (const Control& control : controls)
        {
            if (control.component->GetTooltip().empty() || !control.component->IsVisible()) continue;
            if (control.body.IsHovered() || (control.labelCell.IsValid() && control.labelCell.IsHovered()))
            {
                hovered = &control;
                break;
            }
        }
    }
    if (hovered == nullptr)
    {
        tooltip.SetVisible(false);
        return;
    }

    tooltipText.SetText(hovered->component->GetTooltip());

    // The control and its label together.
    Element::Rect item = hovered->body.GetRect();
    if (hovered->labelCell.IsValid())
    {
        Element::Rect label = hovered->labelCell.GetRect();
        int right = item.x + item.w;
        item.x = std::min(item.x, label.x);
        item.w = right - item.x;
    }

    int tooltipWidth = Units(TOOLTIP_WIDTH);
    int tooltipHeight = tooltip.GetRect().h;
    int gap = Units(TOOLTIP_GAP);
    int roomLeft = item.x - gap;
    int roomRight = static_cast<int>(width) - (item.x + item.w) - gap;

    // The outer side of a multi-column grid is preferred, unless it doesn't fit there.
    bool onLeft = screen.columns > 1 && hovered->labelCell.IsValid() && hovered->column == 0;
    if (onLeft && roomLeft < tooltipWidth && roomRight > roomLeft) onLeft = false;
    else if (!onLeft && roomRight < tooltipWidth && roomLeft > roomRight) onLeft = true;

    int x = onLeft ? item.x - gap - tooltipWidth : item.x + item.w + gap;
    int y = item.y + (item.h - tooltipHeight) / 2;
    x = std::clamp(x, 0, std::max(0, static_cast<int>(width) - tooltipWidth));
    y = std::clamp(y, 0, std::max(0, static_cast<int>(height) - tooltipHeight));

    tooltip.SetInsets(MakeInsets(x, y, OMIT, OMIT));
    tooltip.SetVisible(true);
}

} // namespace Volcano::GUI
