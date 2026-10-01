#ifdef __linux__

#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <execinfo.h>
#include <fstream>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include "OS.hpp"

extern char** environ;

namespace Volcano::Platform {

namespace
{
OS::CrashHandler nativeCrashHandler = nullptr;

void OnFatalSignal(int signal)
{
    std::string reason = std::string("Native crash: ") + strsignal(signal) + " (signal " + std::to_string(signal) + ").";
    if (nativeCrashHandler) nativeCrashHandler(reason);
    std::_Exit(128 + signal);
}

// "binary(_ZN7Volcano3FooEv+0x1a) [0x...]" -> "Volcano::Foo() + 0x1a"
std::string Demangle(const char* frame)
{
    const char* open = std::strchr(frame, '(');
    const char* plus = open ? std::strchr(open, '+') : nullptr;
    if (!open || !plus || plus == open + 1) return frame;

    std::string mangled(open + 1, plus);
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    std::string name = status == 0 && demangled ? demangled : mangled;
    std::free(demangled);

    const char* close = std::strchr(plus, ')');
    return name + " " + std::string(plus, close ? close : plus + std::strlen(plus));
}

bool RunDialogTool(const std::vector<std::string>& args)
{
    std::vector<char*> argv;
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return false;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) != 127;
}
}

uint64_t OS::GetProcessMemoryUsageBytes()
{
    std::ifstream statm("/proc/self/statm");
    uint64_t totalPages = 0, residentPages = 0;
    if (!(statm >> totalPages >> residentPages)) return 0;
    return residentPages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
}

std::string OS::GetCpuModel()
{
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line))
    {
        if (line.rfind("model name", 0) != 0) continue;
        size_t colon = line.find(':');
        return colon == std::string::npos ? "" : line.substr(colon + 1);
    }
    return "";
}

std::string OS::GetExecutablePath()
{
    char path[4096];
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    return length > 0 ? std::string(path, static_cast<size_t>(length)) : "";
}

std::vector<std::string> OS::GetFallbackFontPaths()
{
    // Common distro locations; color-bitmap emoji fonts (NotoColorEmoji)
    // can't be rasterized here, so only outline fonts are listed.
    std::vector<std::string> paths;
    for (const char* path : {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf" })
    {
        if (access(path, R_OK) == 0) paths.emplace_back(path);
    }
    return paths;
}

bool OS::IsDebuggerAttached()
{
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
    {
        if (line.rfind("TracerPid:", 0) == 0) return std::atoi(line.c_str() + 10) != 0;
    }
    return false;
}

void OS::PreciseSleep(int64_t microseconds)
{
    if (microseconds <= 0) return;
    timespec duration{ static_cast<time_t>(microseconds / 1000000), static_cast<long>((microseconds % 1000000) * 1000) };
    while (nanosleep(&duration, &duration) == -1 && errno == EINTR) {}
}

std::string OS::CaptureStackTrace(int skipFrames)
{
    void* frames[48];
    int count = backtrace(frames, 48);
    char** symbols = backtrace_symbols(frames, count);

    std::string trace;
    for (int i = skipFrames + 1; i < count; i++)
    {
        trace += "  " + (symbols ? Demangle(symbols[i]) : std::string("?")) + "\n";
    }
    std::free(symbols);
    return trace;
}

void OS::InstallNativeCrashHandler(CrashHandler handler)
{
    nativeCrashHandler = handler;
    for (int signal : { SIGSEGV, SIGBUS, SIGFPE, SIGILL })
    {
        std::signal(signal, OnFatalSignal);
    }
}

bool OS::LaunchDetached(const std::string& executable, const std::vector<std::string>& args)
{
    std::vector<std::string> all{ executable };
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (const std::string& arg : all) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid;
    return posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv.data(), environ) == 0;
}

void OS::ShowErrorDialog(const std::string& title, const std::string& message)
{
    // No universal Linux dialog API: try the common desktop tools, then stderr.
    if (RunDialogTool({ "zenity", "--error", "--title=" + title, "--text=" + message })) return;
    if (RunDialogTool({ "kdialog", "--title", title, "--error", message })) return;
    std::fprintf(stderr, "%s\n\n%s\n", title.c_str(), message.c_str());
}

bool OS::EnableBufferedRawMouse(GLFWwindow*)
{
    return false; // No batched equivalent; GLFW's XInput2 raw motion is used instead.
}

void OS::ReadBufferedRawMouse(double&, double&)
{}

} // namespace Volcano::Platform

#endif
