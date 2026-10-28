#include "KeyBindings.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include "Config.hpp"
#include "Logger.hpp"

namespace Volcano {

namespace {

// All registered bindings, in registration order (the UI lists them that way).
std::vector<KeyBindingDef>& Registry()
{
    static std::vector<KeyBindingDef> bindings;
    return bindings;
}

struct KeyTable {
    std::vector<std::pair<std::string, uint16_t>> entries; // Display order.
    std::unordered_map<std::string, uint16_t> byLowerName;
    std::unordered_map<uint16_t, std::string> byCode;
    std::vector<uint16_t> codes;
};

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const KeyTable& Keys()
{
    static const KeyTable table = []
    {
        KeyTable t;
        auto add = [&t](const std::string& name, int code)
        {
            uint16_t value = static_cast<uint16_t>(code);
            t.entries.emplace_back(name, value);
            t.byLowerName[Lower(name)] = value;
            t.byCode[value] = name;
            t.codes.push_back(value);
        };

        for (char c = 'A'; c <= 'Z'; c++) add(std::string(1, c), GLFW_KEY_A + (c - 'A'));
        for (char c = '0'; c <= '9'; c++) add(std::string(1, c), GLFW_KEY_0 + (c - '0'));
        for (int i = 1; i <= 25; i++) add("F" + std::to_string(i), GLFW_KEY_F1 + (i - 1));

        add("Space", GLFW_KEY_SPACE);
        add("Enter", GLFW_KEY_ENTER);
        add("Tab", GLFW_KEY_TAB);
        add("Backspace", GLFW_KEY_BACKSPACE);
        add("Insert", GLFW_KEY_INSERT);
        add("Delete", GLFW_KEY_DELETE);
        add("Home", GLFW_KEY_HOME);
        add("End", GLFW_KEY_END);
        add("Page Up", GLFW_KEY_PAGE_UP);
        add("Page Down", GLFW_KEY_PAGE_DOWN);
        add("Up", GLFW_KEY_UP);
        add("Down", GLFW_KEY_DOWN);
        add("Left", GLFW_KEY_LEFT);
        add("Right", GLFW_KEY_RIGHT);
        add("Caps Lock", GLFW_KEY_CAPS_LOCK);
        add("Scroll Lock", GLFW_KEY_SCROLL_LOCK);
        add("Num Lock", GLFW_KEY_NUM_LOCK);
        add("Print Screen", GLFW_KEY_PRINT_SCREEN);
        add("Pause", GLFW_KEY_PAUSE);
        add("Menu", GLFW_KEY_MENU);

        add("Left Shift", GLFW_KEY_LEFT_SHIFT);
        add("Right Shift", GLFW_KEY_RIGHT_SHIFT);
        add("Left Control", GLFW_KEY_LEFT_CONTROL);
        add("Right Control", GLFW_KEY_RIGHT_CONTROL);
        add("Left Alt", GLFW_KEY_LEFT_ALT);
        add("Right Alt", GLFW_KEY_RIGHT_ALT);
        add("Left Super", GLFW_KEY_LEFT_SUPER);
        add("Right Super", GLFW_KEY_RIGHT_SUPER);

        add("'", GLFW_KEY_APOSTROPHE);
        add(",", GLFW_KEY_COMMA);
        add("-", GLFW_KEY_MINUS);
        add(".", GLFW_KEY_PERIOD);
        add("/", GLFW_KEY_SLASH);
        add(";", GLFW_KEY_SEMICOLON);
        add("=", GLFW_KEY_EQUAL);
        add("[", GLFW_KEY_LEFT_BRACKET);
        add("\\", GLFW_KEY_BACKSLASH);
        add("]", GLFW_KEY_RIGHT_BRACKET);
        add("`", GLFW_KEY_GRAVE_ACCENT);

        for (int i = 0; i <= 9; i++) add("Numpad " + std::to_string(i), GLFW_KEY_KP_0 + i);
        add("Numpad .", GLFW_KEY_KP_DECIMAL);
        add("Numpad /", GLFW_KEY_KP_DIVIDE);
        add("Numpad *", GLFW_KEY_KP_MULTIPLY);
        add("Numpad -", GLFW_KEY_KP_SUBTRACT);
        add("Numpad +", GLFW_KEY_KP_ADD);
        add("Numpad Enter", GLFW_KEY_KP_ENTER);

        add("Left Mouse", MOUSE_BUTTON_OFFSET + GLFW_MOUSE_BUTTON_LEFT);
        add("Right Mouse", MOUSE_BUTTON_OFFSET + GLFW_MOUSE_BUTTON_RIGHT);
        add("Middle Mouse", MOUSE_BUTTON_OFFSET + GLFW_MOUSE_BUTTON_MIDDLE);
        for (int button = GLFW_MOUSE_BUTTON_4; button <= GLFW_MOUSE_BUTTON_8; button++)
        {
            add("Mouse" + std::to_string(button + 1), MOUSE_BUTTON_OFFSET + button);
        }
        return t;
    }();
    return table;
}

constexpr const char* NONE = "None";

std::string ConfigKey(const std::string& id) { return "Controls.Keys." + id; }

// Shorthand for the table below.
KeyBindingDef Binding(const char* id, const char* label, const char* category, const char* defaultKey,
    std::vector<KeyBindingAction> actions)
{
    KeyBindingDef def;
    def.id = id;
    def.label = label;
    def.category = category;
    def.defaultKey = defaultKey;
    def.actions = std::move(actions);
    return def;
}

KeyBindingDef AxisBinding(const char* id, const char* label, const char* defaultKey, const char* axis, bool positive)
{
    KeyBindingDef def = Binding(id, label, "Movement", defaultKey, {});
    def.axis = axis;
    def.axisPositive = positive;
    return def;
}

} // namespace

void KeyBindings::Register(KeyBindingDef def)
{
    std::vector<KeyBindingDef>& bindings = Registry();
    auto it = std::find_if(bindings.begin(), bindings.end(), [&def](const KeyBindingDef& b) { return b.id == def.id; });
    if (it != bindings.end()) *it = std::move(def);
    else bindings.push_back(std::move(def));
}

const std::vector<KeyBindingDef>& KeyBindings::All()
{
    return Registry();
}

const KeyBindingDef* KeyBindings::Find(const std::string& id)
{
    for (const KeyBindingDef& def : Registry())
    {
        if (def.id == id) return &def;
    }
    return nullptr;
}

void KeyBindings::RegisterBuiltins()
{
    using T = InputActionTriggerType;

    Register(AxisBinding("MoveForward", "Walk Forwards", "W", "Move.Forward", true));
    Register(AxisBinding("MoveBack", "Walk Backwards", "S", "Move.Forward", false));
    Register(AxisBinding("MoveLeft", "Strafe Left", "A", "Move.Right", false));
    Register(AxisBinding("MoveRight", "Strafe Right", "D", "Move.Right", true));
    // Two actions on one key: "Jump" (PRESS) drives TickLoop's edge-latch so a
    // tap between fixed ticks is never lost, "JumpHold" re-queues a jump every
    // frame the key is down (vanilla's repeat-jump-on-landing).
    Register(Binding("Jump", "Jump", "Movement", "Space", { { "Jump", T::PRESS }, { "JumpHold", T::HOLD } }));
    Register(Binding("Sneak", "Sneak", "Movement", "Left Shift", { { "Sneak", T::HOLD } }));
    Register(Binding("Sprint", "Sprint", "Movement", "Left Control", { { "Sprint", T::HOLD } }));

    Register(Binding("Attack", "Attack/Destroy", "Gameplay", "Left Mouse",
        { { "PrimaryActionPress", T::PRESS }, { "PrimaryAction", T::HOLD } }));
    Register(Binding("Use", "Use Item/Place Block", "Gameplay", "Right Mouse",
        { { "SecondaryAction", T::PRESS }, { "SecondaryActionHold", T::HOLD }, { "SecondaryActionRelease", T::RELEASE } }));
    // Tap drops one item, holding drops the stack (see RenderThread's drop key handling).
    Register(Binding("DropItem", "Drop Selected Item", "Gameplay", "Q",
        { { "DropItem", T::PRESS }, { "DropItemHold", T::HOLD }, { "DropItemRelease", T::RELEASE } }));
    Register(Binding("CyclePerspective", "Cycle Perspective", "Gameplay", "F5", { { "CyclePerspective", T::PRESS } }));

    for (int slot = 0; slot < 9; slot++)
    {
        std::string number = std::to_string(slot + 1);
        Register(Binding(("Hotbar" + number).c_str(), ("Hotbar Slot " + number).c_str(), "Inventory", number.c_str(),
            { { "Hotbar." + std::to_string(slot), T::PRESS } }));
    }
    Register(Binding("ToggleInventory", "Open/Close Inventory", "Inventory", "E", { { "ToggleInventory", T::PRESS } }));

    Register(Binding("OpenChat", "Open Chat", "Interface", "T", { { "OpenChat", T::PRESS } }));
    Register(Binding("OpenCommand", "Open Chat With Command", "Interface", "/", { { "OpenCommand", T::PRESS } }));
    Register(Binding("PlayerList", "Show Player List", "Interface", "Tab", { { "PlayerList", T::HOLD } }));
    Register(Binding("ToggleLighting", "Toggle Lighting", "Interface", "G", { { "ToggleLighting", T::PRESS } }));
    Register(Binding("ReloadShaders", "Reload Shaders", "Interface", "F9", { { "ReloadShaders", T::PRESS } }));
}

std::string KeyBindings::GetKey(Config& config, const std::string& id)
{
    const KeyBindingDef* def = Find(id);
    if (def == nullptr) return NONE;
    return std::get<std::string>(config.Get(ConfigKey(id), std::string(def->defaultKey)));
}

void KeyBindings::Apply(Config& config, InputHandler& input)
{
    if (Registry().empty()) RegisterBuiltins();

    for (const KeyBindingDef& def : Registry())
    {
        std::string name = GetKey(config, def.id);
        uint16_t code = 0;
        bool bound = KeyCode(name, code);
        if (!bound && Lower(name) != Lower(NONE))
        {
            Log::Error("Unknown key \"" + name + "\" for " + def.label + " in settings.json; using " + def.defaultKey + ".");
            bound = KeyCode(def.defaultKey, code);
        }

        for (const KeyBindingAction& action : def.actions)
        {
            input.RegisterAction(action.action, action.trigger, bound ? std::vector<uint16_t>{ code } : std::vector<uint16_t>{});
        }
        if (!def.axis.empty()) input.SetAxisKey(def.axis, def.axisPositive, bound ? code : uint16_t{ 0 });
    }
}

bool KeyBindings::Rebind(Config& config, InputHandler& input, const std::string& id, const std::string& keyName)
{
    if (Find(id) == nullptr) return false;

    uint16_t code = 0;
    if (Lower(keyName) == Lower(NONE)) config.Set(ConfigKey(id), std::string(NONE));
    else if (KeyCode(keyName, code)) config.Set(ConfigKey(id), KeyName(code)); // Stored in canonical spelling.
    else return false;

    Apply(config, input);
    return true;
}

void KeyBindings::Reset(Config& config, InputHandler& input, const std::string& id)
{
    if (const KeyBindingDef* def = Find(id)) Rebind(config, input, id, def->defaultKey);
}

void KeyBindings::ResetAll(Config& config, InputHandler& input)
{
    for (const KeyBindingDef& def : Registry()) config.Set(ConfigKey(def.id), std::string(def.defaultKey));
    Apply(config, input);
}

std::vector<std::string> KeyBindings::Conflicts(Config& config, const std::string& id)
{
    std::vector<std::string> labels;
    std::string key = Lower(GetKey(config, id));
    if (key == Lower(NONE)) return labels;

    for (const KeyBindingDef& other : Registry())
    {
        if (other.id != id && Lower(GetKey(config, other.id)) == key) labels.push_back(other.label);
    }
    return labels;
}

std::string KeyBindings::KeyName(uint16_t code)
{
    auto it = Keys().byCode.find(code);
    return it != Keys().byCode.end() ? it->second : std::string(NONE);
}

bool KeyBindings::KeyCode(const std::string& name, uint16_t& code)
{
    auto it = Keys().byLowerName.find(Lower(name));
    if (it == Keys().byLowerName.end()) return false;
    code = it->second;
    return true;
}

const std::vector<uint16_t>& KeyBindings::BindableCodes()
{
    return Keys().codes;
}

} // namespace Volcano
