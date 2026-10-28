#pragma once
#ifndef VOLCANO_KEY_BINDINGS_H
#define VOLCANO_KEY_BINDINGS_H

#include <cstdint>
#include <string>
#include <vector>
#include "InputHandler.hpp"

namespace Volcano {

class Config;

// One thing InputHandler should answer for when a binding's key is active.
struct KeyBindingAction {
    std::string action;                // The InputHandler action name, e.g. "Jump".
    InputActionTriggerType trigger;
};

// A rebindable control. The player's choice is stored in settings.json as
// Controls.Keys.<id> (a key name, see KeyBindings::KeyName); `defaultKey` is
// what it is until they change it.
struct KeyBindingDef {
    std::string id;         // Stable, no dots: it is a settings.json key.
    std::string label;      // Shown in the Key Binds screen.
    std::string category;   // Groups rows in that screen.
    std::string defaultKey; // A KeyName, or "None".

    // Actions registered on the bound key.
    std::vector<KeyBindingAction> actions;

    // Or: one end of a KeyPair axis (Move.Forward's W/S, Move.Right's D/A).
    std::string axis;
    bool axisPositive = true;
};

// Rebindable keys: the registry, their settings.json storage and the key
// name table. Render thread only (Config isn't thread-safe). Mods add their
// own controls with Register, before or after Apply (call Apply again after).
class KeyBindings {
public:
    // Adds a binding, or replaces one with the same id.
    static void Register(KeyBindingDef def);
    static const std::vector<KeyBindingDef>& All();
    static const KeyBindingDef* Find(const std::string& id);

    // The built-in controls, with vanilla Minecraft defaults. Called once by Apply.
    static void RegisterBuiltins();

    // Reads every binding from `config` (a missing or unreadable name falls
    // back to the default, logging the latter) and registers it with `input`.
    static void Apply(Config& config, InputHandler& input);

    // The key name a binding currently uses ("None" if unbound).
    static std::string GetKey(Config& config, const std::string& id);

    // Binds `id` to `keyName` ("None" unbinds), saves it and re-applies
    // everything. Returns false for an unknown id or key name.
    static bool Rebind(Config& config, InputHandler& input, const std::string& id, const std::string& keyName);

    // Back to the default key.
    static void Reset(Config& config, InputHandler& input, const std::string& id);
    static void ResetAll(Config& config, InputHandler& input);

    // Other bindings that currently use the same key as `id` (labels), for a conflict warning.
    static std::vector<std::string> Conflicts(Config& config, const std::string& id);

    // Key names are readable and stable: "Q", "1", "Left Alt", "Space",
    // "F9", "Mouse1"... "None" is unbound and has no code.
    static std::string KeyName(uint16_t code);
    static bool KeyCode(const std::string& name, uint16_t& code); // Case-insensitive; false if unknown.

    // Every code that can be bound, in the order a UI would list them.
    static const std::vector<uint16_t>& BindableCodes();
};

} // namespace Volcano

#endif
