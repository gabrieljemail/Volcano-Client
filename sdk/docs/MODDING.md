# Writing a Volcano mod

A mod is a DLL (`.so` on Linux) in `mods/`. The client loads each one at start-up, in file-name order, and calls its `VC_InitMod`. There are two ways to talk to the client, and a mod can use either or both:

1. **C++ against the client's own classes.** Every `Volcano::` function is exported from the exe, so a mod can call `GlobalState`, `Config`, `World`, `GUIController`, `RenderEngine`... directly, and can detour any of them with MinHook.
2. **Plain C events.** `VC_EventHandler` / `VC_EventHandlerEx` need nothing from C++. Integrations in other languages can use only these.

The API lives next to the code it belongs to, not in a separate layer. The sections below say where.

## Entry points (`src/mods/VolcanoModAPI.h`)

```cpp
VC_MOD_EXPORT const VC_ModInfo* VC_GetModInfo();            // optional: id, name, version, API version
VC_MOD_EXPORT void VC_InitMod(Volcano::GlobalState* game);   // required: register handlers here
VC_MOD_EXPORT void VC_ShutdownMod(Volcano::GlobalState*);    // optional: undo everything before unload
```

`VC_GetModInfo` lets the client refuse a mod built for another API generation (`VC_API_VERSION_MAJOR` differs, or the mod wants a newer minor). Changing `GlobalState`'s layout is a breaking change, so the major is bumped when it happens.

Anything a mod hands to the client (event handlers, HUD elements, listeners, render passes) points into the DLL, so remove it in `VC_ShutdownMod`.

## Events

| Event | Thread | Payload |
|---|---|---|
| `PRE_START`, `SPLASH_SCREEN`, `TEXTURES_LOADED`, `AUDIO_LOADED`, `RENDER_START`, `CHUNKS_READY`, `LEAVE_PLAY_STATE`, `PRE_QUIT` | main | none |
| `ENTER_CONFIG_STATE`, `ENTER_PLAY_STATE` | network | none |
| `FRAME_BEGIN` | render | none |
| `TICK` | tick | none |
| `CHAT_RECEIVED` | network | `text`: the message as plain text, sender in `<>` |
| `KEY_PRESSED` | render | `a`: GLFW key code |
| `BLOCK_CHANGED` | meshing | `a`,`b`,`c`: block position, `d`: new visual id |
| `HUD_RENDER` | render, inside the ImGui frame | none |

Things exist only after their event: `renderEngine` after `SPLASH_SCREEN`, `input` after `TEXTURES_LOADED`, `audioManager` after `AUDIO_LOADED`. Register in `VC_InitMod`; do the work in the handler.

Handlers run on the thread in the table. Do not call ImGui or Vulkan from a non-render thread, and keep network/tick handlers short.

## Display (`GUIController`, `src/renderer/gui/`)

- `AddHudElement(name, fn)` / `RemoveHudElement(name)`: `fn` runs every frame inside the ImGui frame, after the vanilla HUD, while no screen is open. Use any ImGui call.
- `DrawItem(drawList, pos, stack)`, `GetItemIconTexture(stack)`: the same item icons and counts the hotbar uses.
- `GetArtScale()`, `GetHotbarOrigin()`: so widgets line up with the vanilla layout.
- `CreateWindow(...)` and `GUIWindow::customContent`: a window of components plus free-form ImGui.
- Standard controls, all the main menu's height and look, with no layout math at the call site: `DrawButton(label, width)`, `DrawMenuButton(label)` (centered, menu width), `DrawInput(id, text, width, hint)` (returns true on Enter) and `DrawDropdown(id, options, selected, width)`. `GetControlHeight()`, `CenterNextControl()` and `AlignTextToControl()` (for a caption beside a control) cover the rest. Use these instead of raw `ImGui::Button`/`InputText`, which are shorter and look different.
- `SetDebugDisplay(...)`: lines in the F3 overlay.
- `InventoryManager::TakeSnapshot()` (`GlobalState::inventory`): a lock-free copy of the hotbar, main inventory, armor, offhand and carried item. Read this instead of locking `inventory.mutex` yourself.

## Packets (`src/network/models/PacketEvent.hpp`)

`PacketEvents::AddListener(fn)` sees every play-state packet before the client handles it (clientbound, network thread) or sends it (serverbound, the thread that queued it). Set `event.cancelled` to drop it, or edit `event.payload`. Packet ids are in `src/network/PacketIds.hpp`. Login and configuration packets are never tapped.

## Rendering (`src/renderer/engine/`)

Derive from `Engine::RenderPass`, then `renderEngine->AddPass / InsertPassBefore / InsertPassAfter` (see `docs/RenderingEngineRework.md`). The Vulkan handles a pass needs are public methods on `renderEngine->GetVulkan()`. A pass's `GetStage()` picks `PRE_MAIN`, `MAIN` or `OVERLAY`.

## Sound (`src/audio/`)

- `AudioManager::AddSoundListener(fn)`: every sound goes through `PlaySound`, so a listener sees them all and can change the volume, pitch or name, or cancel it.
- `SoundRegistry::Register(name, path)`: add your own `.ogg` under a name like `mymod:ui/click`.

## Key bindings (`src/KeyBindings.hpp`)

Controls are rebindable by the player (Settings -> Controls -> Key Binds, or `Controls.Keys.<id>` in settings.json). `KeyBindings::Register({ id, label, category, defaultKey, actions })` adds your own: the key then shows up in that screen, and `input->IsActive("YourAction")` / `WasActivated` follow whatever the player chose. Register before `KeyBindings::Apply`, or call `Apply(*game->config, *game->input)` again after (render thread). Key names are readable (`"Q"`, `"Left Alt"`, `"Mouse1"`, `"None"`); `KeyBindings::KeyName` / `KeyCode` convert.

## Hooking internals with MinHook

Every non-inline `Volcano::` function is exported, so MinHook can detour it by address (`GetProcAddress` on the exe with the mangled name, or a declaration plus the import library). The API above is the stable route; a detour is for anything it does not cover and may break when that function changes. Keep hot paths in the client non-inline so they stay hookable.

## Data a mod reads

`GlobalState` is the access point; its fields are public. Cross-thread fields come with a mutex named `<field>Mutex` (see `CODESTYLE.md`). Prefer the snapshot-style helpers where they exist. `World` locks itself. `Config` is not thread-safe: read it from the render thread or at init.

If a polymorphic class (one with virtual functions) is meant to be derived from in a mod, mark it `VC_API` in its header (as `RenderPass` is): the exe's exports cannot carry its vtable otherwise.
