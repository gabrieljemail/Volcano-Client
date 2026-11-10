# Volcano Client
Volcano is a Minecraft client written in C++. It is designed to be fast, customizable and moddable. This document has three sections below, each for a different audience that might be reading this.

> Volcano Client is not an official Minecraft product. It is not approved by or associated with Mojang or Microsoft.

## Players
If you are a player, you'll be happy to know that using Volcano is actually simpler than Minecraft!
To get started, follow these steps:
	1. Click Releases on the right of this page on GitHub. You'll find a bunch of versions, just use the top one.
	2. Download the distribution for your platform, e.g. VolcanoClient-Installer.exe for Windows or VolcanoClient.tgz for Linux.
		a. If you are on Windows, just click the .exe file.
		b. If you are on Linux, extract the .tgz file to somewhere you won't accidentally delete it. Then, you can launch it from your terminal by typing the path to VolcanoClient.elf or make a .desktop file for it. If it doesn't work the first time, try running this in your terminal: `chmod +x <PATH_TO_VOLCANO_ELF>`.
	3. Launch the game, and the game will guide you through initial setup. The first time, it asks to download the game's textures, models and sounds directly from Mojang's servers (they aren't included with Volcano). You need to own a valid license for the game to do this, and the download only happens if you agree.

No launcher is necessary, and you'll be happy to know that your resource packs will work in Volcano!
	- **To use a 26.2+ resource pack:** Copy the ZIP file (or folder) to the `resourcepacks` folder next to the game. To use it, launch the client, go to Settings, then \[Resource Packs\], and add it. Packs higher in the list win over lower ones, and the game's own assets always sit at the bottom.
	- **Using mods:** Unfortunately, your Minecraft mods won't work for Volcano. However, Volcano is still moddable. If there is a mod you want to install, download its .dll (or .so) file and put it in `<YOUR_VOLCANO_INSTALL>/mods`. **BE CAREFUL WITH MODS, malicious ones can harm your computer, steal information, give you a virus, or eat your pet rock.**
	- **Singleplayer:** If you came back here because you found the text that shows up while your world takes forever to load and thought it was scary, it's okay! That text is the server log, and that tells you what it's doing. Minecraft doesn't use a dedicated server like Volcano, so it doesn't show you any logs; but Volcano shows you these logs in case something goes wrong and you need to know what the server was doing.

## Modders
A Volcano mod is a native library (a `.dll` on Windows, a `.so` on Linux) that the client loads from the `mods` folder at startup. Mods are written in C++20. They can listen for events, add HUD widgets, react to network packets and sounds, read and write config, register key bindings and add render passes.

> The mod API is pre-alpha. It can change between versions, and a mod built for one version may need rebuilding for the next. The client checks the API version a mod was built with when it loads it.

**Build the example mod** (it's in `sdk/examples/hello_mod`):
```powershell
cmake -DVOLCANO_BUILD_EXAMPLES=ON .
powershell .\ninja.ps1
```
`mods/hello_mod.dll` appears. Start the client and look for `[+] hello_mod.dll` in the mod summary. To build your own, use `volcano_add_mod(name SOURCES ...)` from `sdk/VolcanoMod.cmake`; it builds with the client's flags and puts the result in `mods/`.

**Rules a mod must follow.** A mod shares `std::` types, memory allocations and ImGui with the client, so it has to match how the client is built:
- Build with clang, the dynamic MSVC runtime (`/MD`) and C++20.
- Define `ImDrawIdx=unsigned int`, because the client's ImGui draw lists use 32-bit indices. `volcano_add_mod` sets this for you.
- Use the client's own Vulkan, GLFW, ImGui and glm headers. Don't bring a second copy.
- Pass the API version from `src/mods/VolcanoModAPI.h` (`VC_API_VERSION_MAJOR` and `VC_API_VERSION_MINOR`) when your mod registers.

**Where things are:**
- `sdk/docs/MODDING.md`: what a mod can do, and where each part of the API lives.
- `src/mods/VolcanoModAPI.h`: the entry points and version.
- `sdk/README.md` and `sdk/examples/hello_mod/`: a working example using events, a HUD widget, packet and sound listeners, config and a render pass.

Mods run with the same permissions as the game, so only run mods from people you trust.

## Contributors
Thanks for wanting to help. Volcano is a Minecraft client written from scratch in C++20 and Vulkan, with a UI built on [Pincel](https://github.com/gabrieljemail/Pincel), its own UI library written in C++ and Rust.

**Build it**
1. Install clang and lld, CMake 3.20 or newer, Ninja, the Vulkan SDK (it includes `glslangValidator`, which compiles the shaders), OpenSSL, and Rust (for Pincel).
2. Clone both repositories next to each other, so Pincel is at `../Pincel`:
   ```
   Volcano-Client/
   Pincel/
   ```
3. Build Pincel first: configure it in its own folder, then build the `pincel` target.
4. Configure and build the client. GLFW, ZLIB, Asio, OpenAL and glm are found on your system or downloaded automatically (see `cmake/Dependencies.cmake`); ImGui, vk-bootstrap, VMA, nlohmann/json and stb are in `third_party/`. OpenSSL must be installed on your system.

The workflow in `.github/workflows/ci.yml` is a working reference of the exact steps on Windows and Linux. Linux support is still experimental.

**Where to look**
- `src/renderer/`: rendering (terrain, entities, particles, the hand) and the GUI.
- `src/network/`: the Minecraft protocol, packets and the command tree.
- `src/platform/`: everything operating-system specific. OS headers are only allowed in the `OS*.cpp` files here.
- `STATE.md`: what is done, what is next, and known bugs. Check it before starting something, in case it's already being worked on.

**Rules**
- Read `CODESTYLE.md` before writing code. It covers formatting, naming and comments (short ones; no long essays).
- The client has to build and run on both Windows and Linux.
- **Never commit Mojang's code or assets**: no decompiled source, extracted jars or resource files. The client downloads the game's assets from Mojang at first launch instead, and the repo's `.gitignore` excludes them. Third-party resource packs and build outputs stay out of the repo as well.
- Keep the client compatible with the server's expected behavior: player movement and physics must match vanilla's exact arithmetic, or servers will reject positions.

**Contributing**
- For anything larger than a small fix, open an issue first so we can agree on the approach.
- Keep pull requests small and focused, and make sure CI passes.

**License:** not chosen yet. Until one is added, ask in an issue before reusing any of the code.
