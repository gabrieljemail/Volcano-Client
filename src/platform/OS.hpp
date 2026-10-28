#pragma once
#ifndef VOLCANO_PLATFORM_OS_H
#define VOLCANO_PLATFORM_OS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace Volcano::Platform {

// The one place OS headers (<windows.h>, POSIX/Linux APIs) are allowed.
// Implemented per platform in OSWindows.cpp / OSLinux.cpp; everything else
// in the client calls through here.
class OS
{
public:
    using CrashHandler = void (*)(const std::string& reason);

    // Private (committed) bytes of this process. Not the working set, which
    // Windows trims under memory pressure and reads absurdly low.
    static uint64_t GetProcessMemoryUsageBytes();
    // Physical RAM the system could hand out right now.
    static uint64_t GetAvailablePhysicalMemoryBytes();
    static std::string GetCpuModel();
    static std::string GetExecutablePath();
    // Installed system fonts for characters the UI font lacks (symbols, box
    // drawing, emoji), in priority order. Only paths that exist.
    static std::vector<std::string> GetFallbackFontPaths();
    static bool IsDebuggerAttached();

    // Raises this process's scheduling priority one step above normal
    // (Windows: ABOVE_NORMAL_PRIORITY_CLASS; Linux: nice -5, which needs
    // privileges and is skipped otherwise), so background apps compete less
    // with the render thread on low-core machines. Returns whether it took effect.
    static bool SetAboveNormalPriority();

    // Sleeps with sub-millisecond precision (std::this_thread::sleep_for
    // rounds up to Windows' 15.6 ms scheduler tick, capping FPS at ~64).
    static void PreciseSleep(int64_t microseconds);

    // Symbolized call stack of the calling thread, one frame per line.
    static std::string CaptureStackTrace(int skipFrames);

    // Routes native crashes (access violation / SIGSEGV, SIGBUS, SIGFPE,
    // SIGILL) to handler with a short description.
    static void InstallNativeCrashHandler(CrashHandler handler);

    // Starts a process that outlives this one. Returns false on failure.
    static bool LaunchDetached(const std::string& executable, const std::vector<std::string>& args);

    // Windows GUI builds start without a console. When launched from a terminal this joins it, so logs still print
    // there; with output already redirected, or a double click, it does nothing. A no-op elsewhere. Call first.
    static void AttachParentConsole();

    // Blocking error dialog. Falls back to stderr where no dialog is available.
    static void ShowErrorDialog(const std::string& title, const std::string& message);

    // Downloads `url` (https) into `body`, following redirects and checking the server's certificate against the system's
    // trust store. `progress(received, total)` runs as data arrives (total is 0 when unknown). Blocking, thread safe.
    // Returns false with a reason in `error`.
    static bool HttpGet(const std::string& url, std::vector<uint8_t>& body, std::string& error,
        const std::function<void(uint64_t received, uint64_t total)>& progress = nullptr);

    // Blocking yes/no prompt. Returns true for the agree answer. `agreeLabel` names it in the text where the dialog
    // can't relabel its buttons; without a dialog it asks on the terminal.
    static bool ShowConsentDialog(const std::string& title, const std::string& message, const std::string& agreeLabel);

    // Buffered raw mouse input (Windows: GetRawInputBuffer, one batched read
    // per frame instead of one message per mouse report). Returns false where
    // unsupported; callers then fall back to GLFW's own raw motion.
    static bool EnableBufferedRawMouse(GLFWwindow* window);
    // Adds all raw motion since the last call to dx/dy. Call right before
    // glfwPollEvents, on the window's thread.
    static void ReadBufferedRawMouse(double& dx, double& dy);

    // Mod library extension for this platform, with the dot (".dll" / ".so").
    static const char* GetObjectExtension();
    // Loads a DLL / shared object by path. Returns an opaque handle, or
    // nullptr if it failed to load or is the wrong type for this platform.
    static void* LoadObject(const std::string& path);
    // Address of an exported symbol, or nullptr.
    static void* GetObjectSymbol(void* handle, const std::string& name);
    static void UnloadObject(void* handle);
};

} // namespace Volcano::Platform

#endif
