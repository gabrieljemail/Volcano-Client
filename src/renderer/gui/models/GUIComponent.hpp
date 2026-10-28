#pragma once
#ifndef GUI_COMPONENT_H
#define GUI_COMPONENT_H

#include <string>
#include <vector>
#include <functional>

namespace Volcano::GUI {

enum GUIComponentType {
    TEXT,
    BUTTON,
    INPUT,
    SLIDER,
    LIST,
    NUMBER,
    OPTION
};

class GUIComponent {
public:
    GUIComponent(GUIComponentType type, const std::string& label = "") : type(type), label(label) {}
    virtual ~GUIComponent() = default;

    void Render();
    void Remove();

    void AddChild(GUIComponent* child) { children.push_back(child); }
    void RemoveChild(GUIComponent* ptr) { std::erase(children, ptr); if (ptr) delete ptr; }
    void SetLabel(const std::string& newLabel) { label = newLabel; }
    // Shown beside the component on hover (see GUIController::RenderScreen).
    void SetTooltip(const std::string& text) { tooltip = text; }
    const std::string& GetTooltip() const { return tooltip; }
    void SetVisibile(bool visibility) { visible = visibility; }
    void SetParent(GUIComponent* newParent) { parent = newParent; parent->AddChild(this); }

    GUIComponentType GetType() { return type; }
    std::string GetLabel() { return label; }
    std::vector<GUIComponent*> GetChildren() { return children; }
    bool IsVisible() { return visible; }
    GUIComponent* GetParent() { return parent; }

    // Polymorphic virtuals.
    virtual void AddClickHandler(std::function<void()>* callback) {}
    virtual void RemoveClickHandler([[maybe_unused]] std::function<void()>* callback) {}
    // virtual void SetHoverState(ButtonHoverState newHoverState) {}
    virtual void Click() {}

    // Overridden by text-editable components (e.g. TextInput) so the
    // GUIController can render them generically without knowing the
    // concrete component type.
    virtual char* GetTextBuffer() { return nullptr; }
    virtual size_t GetTextBufferCapacity() { return 0; }

private:
    GUIComponentType type;
    bool visible = true;
    std::string label;
    std::string tooltip;
    std::vector<GUIComponent*> children;
    GUIComponent* parent = nullptr;
};

} // namespace Volcano

#endif
