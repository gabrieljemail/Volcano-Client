# Volcano Client — notes for Claude

Read `CODESTYLE.md` before writing code, and `STATE.md` for current status and the roadmap.

## Build
```powershell
powershell .\ninja.ps1
```
Run from the repo root. The `build\` tree is a stale second config; ignore it. The exe can't be relinked while the client is running.

## Live testing
The client runs on the user's real desktop, so be gentle with it.

- **Launch once per check.** Never kill-and-relaunch in a loop: that has wedged the Intel UHD driver (every new Vulkan window stayed white until the next day). Close with `CloseMainWindow()`, not `Stop-Process`, so shutdown runs.
- Use windowed mode (`Window.Mode: 0` in `settings.json`) for tests.
- Wait for `Meshing thread created` in the log before sending input; clicks during the splash screen land on the wrong screen later.
- Once the user is in the game, stop sending input (they may be typing in chat).

```powershell
# Launch, logging stdout/stderr
Start-Process .\VolcanoClient.exe -WorkingDirectory (Get-Location) -RedirectStandardOutput client.log -RedirectStandardError client.err

# Screenshot: press F2 in the client; it writes screenshots\<date_time>.png
# (the PNG is written async, so poll for the new file). Read that file to see
# the frame, HUD included. Window-capture APIs return white for the Vulkan
# swapchain on this machine, so don't use `ui.ps1 shot` for frame content.
powershell -File tools\ui.ps1 key F2

# Click at F2-screenshot pixel coordinates (client area). Works on ImGui.
powershell -File tools\ui.ps1 click "426,276"

# Real-scancode keys: Up Down Left Right Tab Space Enter Esc F2 F3 F11
powershell -File tools\ui.ps1 key "Space"
```

- Prefer `click` for menus. `key` Space/Enter activate the focused button, but ImGui arrow/Tab navigation currently doesn't move focus (open bug).
- `ui.ps1 keys` (SendKeys) sends arrows without the extended-key flag, which GLFW reads as numpad keys; use `key` instead.
- `crash-report.txt` next to the exe has the reason and stack trace of the last crash.
