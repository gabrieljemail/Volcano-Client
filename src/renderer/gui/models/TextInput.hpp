#pragma once
#ifndef GUI_TEXT_INPUT_H
#define GUI_TEXT_INPUT_H

#include <algorithm>
#include <string>
#include <vector>
#include "GUIComponent.hpp"

namespace Volcano::GUI {

// Single-line editable text field, backed by a fixed-size buffer since
// ImGui::InputText edits the buffer directly rather than a std::string.
class TextInput : public GUIComponent {
public:
    explicit TextInput(const std::string& label, const std::string& initialValue = "", size_t capacity = 256)
        : GUIComponent(GUIComponentType::INPUT, label), buffer(capacity, '\0')
    {
        SetValue(initialValue);
    }

    std::string GetValue() const { return std::string(buffer.data()); }
    void SetValue(const std::string& value)
    {
        size_t n = std::min(value.size(), buffer.size() - 1);
        std::copy_n(value.begin(), n, buffer.begin());
        buffer[n] = '\0';
    }

    char* GetTextBuffer() override { return buffer.data(); }
    size_t GetTextBufferCapacity() override { return buffer.size(); }

private:
    std::vector<char> buffer;
};

} // namespace Volcano::GUI

#endif
