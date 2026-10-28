# Volcano mod SDK

- `docs/MODDING.md`: what a mod can do and where each API lives.
- `VolcanoMod.cmake`: `volcano_add_mod(name SOURCES ...)` builds a mod with the client's flags into `mods/`.
- `examples/hello_mod/`: a working mod using events, a HUD widget, packet and sound listeners, config and a render pass.

## Build the example

```powershell
cmake -DVOLCANO_BUILD_EXAMPLES=ON .
powershell .\ninja.ps1
```

`mods/hello_mod.dll` appears; start the client and look for `[+] hello_mod.dll` in the mod summary.

## Rules a mod must follow

The client is built with clang, the dynamic MSVC runtime (`/MD`) and C++20. A mod must match, because it shares `std::` types, allocations and ImGui with the exe. `VolcanoMod.cmake` sets the matching flags; the one that is easy to miss is `ImDrawIdx=unsigned int` (ImGui draw-list indices are 32-bit).

The client's own Vulkan, GLFW, ImGui and glm headers are the ones to include; do not bring a second copy.
