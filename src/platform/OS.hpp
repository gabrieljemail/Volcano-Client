#pragma once
#ifndef VOLCANO_PLATFORM_OS_H
#define VOLCANO_PLATFORM_OS_H

#include <cstdint>
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

    // Blocking error dialog. Falls back to stderr where no dialog is available.
    static void ShowErrorDialog(const std::string& title, const std::string& message);

    // Buffered raw mouse input (Windows: GetRawInputBuffer, one batched read
    // per frame instead of one message per mouse report). Returns false where
    // unsupported; callers then fall back to GLFW's own raw motion.
    static bool EnableBufferedRawMouse(GLFWwindow* window);
    // Adds all raw motion since the last call to dx/dy. Call right before
    // glfwPollEvents, on the window's thread.
    static void ReadBufferedRawMouse(double& dx, double& dy);

    // Loads a dynamic link library (DLL) or shared object (SO) by path, with the extension.
    // Returns `false` if the library failed to load or was the wrong type for this platform.
    static bool LoadObject(const std::string& path);
};

} // namespace Volcano::Platform

#endif
