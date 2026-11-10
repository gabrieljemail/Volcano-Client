#ifdef __linux__

#include <cctype>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fstream>
#include <iterator>
#include <spawn.h>
#include <string>
#include <sys/resource.h>
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

// Runs a dialog tool and returns its exit code (0 is Yes), or -1 when it isn't installed.
int RunDialogToolStatus(const std::vector<std::string>& args)
{
    std::vector<char*> argv;
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) == 127) return -1;
    return WEXITSTATUS(status);
}
}

uint64_t OS::GetProcessMemoryUsageBytes()
{
    std::ifstream statm("/proc/self/statm");
    uint64_t totalPages = 0, residentPages = 0;
    if (!(statm >> totalPages >> residentPages)) return 0;
    return residentPages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
}

uint64_t OS::GetAvailablePhysicalMemoryBytes()
{
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    uint64_t kilobytes = 0;
    while (meminfo >> key >> kilobytes) {
        if (key == "MemAvailable:") return kilobytes * 1024;
        meminfo.ignore(256, '\n');
    }
    return 0;
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

bool OS::SetMemoryPriority(int)
{
    return false; // No per-process page priority on Linux.
}

bool OS::SetAboveNormalPriority()
{
    // Lowering nice below 0 needs CAP_SYS_NICE (or an rlimit); without it this just fails.
    return setpriority(PRIO_PROCESS, 0, -5) == 0;
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

const char* OS::GetObjectExtension()
{
    return ".so";
}

bool OS::HttpRequest(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
    const std::string& requestBody, int& status, std::vector<uint8_t>& response, std::string& error,
    const std::function<void(uint64_t, uint64_t)>& progress)
{
    response.clear();
    status = 0;
    if (url.rfind("https://", 0) != 0 || url.find_first_of("\"\\\r\n") != std::string::npos)
    {
        error = "Not a valid https address: " + url;
        return false;
    }

    // Headers and body go through files (mode 0600), not argv, since they carry tokens.
    char configName[] = "/tmp/volcano-http-XXXXXX";
    char bodyName[] = "/tmp/volcano-body-XXXXXX";
    char outName[] = "/tmp/volcano-out-XXXXXX";
    int configFd = mkstemp(configName);
    int bodyFd = mkstemp(bodyName);
    int outFd = mkstemp(outName);
    auto cleanup = [&]
    {
        for (int fd : { configFd, bodyFd, outFd })
        {
            if (fd >= 0) close(fd);
        }
        unlink(configName);
        unlink(bodyName);
        unlink(outName);
    };
    if (configFd < 0 || bodyFd < 0 || outFd < 0)
    {
        cleanup();
        error = "Couldn't create temporary files.";
        return false;
    }

    auto escape = [](const std::string& text)
    {
        std::string out;
        for (char c : text)
        {
            if (c == '\r' || c == '\n') continue;
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out;
    };
    std::string config = "url = \"" + url + "\"\nrequest = \"" + escape(method) + "\"\nlocation\nmax-redirs = 5\nsilent\n"
        "show-error\noutput = \"" + outName + "\"\nwrite-out = \"%{http_code}\"\n";
    for (const std::string& header : headers) config += "header = \"" + escape(header) + "\"\n";
    if (!requestBody.empty())
    {
        if (write(bodyFd, requestBody.data(), requestBody.size()) < 0)
        {
            cleanup();
            error = "Couldn't write the request.";
            return false;
        }
        config += std::string("data-binary = \"@") + bodyName + "\"\n";
    }
    if (write(configFd, config.data(), config.size()) < 0)
    {
        cleanup();
        error = "Couldn't write the request.";
        return false;
    }

    FILE* pipe = popen((std::string("curl -K ") + configName + " 2>/dev/null").c_str(), "r");
    if (pipe == nullptr)
    {
        cleanup();
        error = "Couldn't run curl; install it to sign in.";
        return false;
    }
    char code[16]{};
    size_t codeLength = fread(code, 1, sizeof(code) - 1, pipe);
    int exitStatus = pclose(pipe);
    status = codeLength > 0 ? std::atoi(code) : 0;

    std::ifstream file(outName, std::ios::binary);
    response.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    cleanup();
    if (exitStatus != 0 || status == 0)
    {
        error = "The request failed (curl exit " + std::to_string(exitStatus) + ").";
        return false;
    }
    if (progress) progress(response.size(), response.size());
    return true;
}

bool OS::HttpGet(const std::string& url, std::vector<uint8_t>& body, std::string& error,
    const std::function<void(uint64_t, uint64_t)>& progress)
{
    body.clear();

    // Only plain https addresses reach the shell, so nothing in the URL can be read as a command.
    bool safe = url.rfind("https://", 0) == 0;
    for (char c : url)
    {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || std::strchr("-._~:/?&=%", c) != nullptr)) safe = false;
    }
    if (!safe)
    {
        error = "Not a valid https address: " + url;
        return false;
    }

    // curl checks certificates against the system's CA store, which is the point.
    FILE* pipe = popen(("curl -fsSL --max-redirs 5 '" + url + "' 2>/dev/null").c_str(), "r");
    if (pipe == nullptr)
    {
        error = "Couldn't run curl; install it to download.";
        return false;
    }
    uint8_t buffer[65536];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
    {
        body.insert(body.end(), buffer, buffer + read);
        if (progress) progress(body.size(), 0);
    }
    int status = pclose(pipe);
    if (status != 0 || body.empty())
    {
        error = "The download failed (curl exit " + std::to_string(status) + ").";
        body.clear();
        return false;
    }
    return true;
}

bool OS::ShowYesNoDialog(const std::string& title, const std::string& message)
{
    // Desktop tools first (exit code 0 is Yes), then the terminal.
    int answer = RunDialogToolStatus({ "zenity", "--question", "--title=" + title, "--text=" + message });
    if (answer < 0) answer = RunDialogToolStatus({ "kdialog", "--title", title, "--yesno", message });
    if (answer >= 0) return answer == 0;
    std::fprintf(stderr, "%s

%s

Type \"yes\" to accept, anything else to decline: ", title.c_str(), message.c_str());
    char answer[16] = {};
    if (std::fgets(answer, sizeof(answer), stdin) == nullptr) return false;
    return std::strncmp(answer, "yes", 3) == 0;
}

bool OS::ShowConsentDialog(const std::string& title, const std::string& message, const std::string& agreeLabel)
{
    std::fprintf(stderr, "%s\n\n%s\n\nType \"yes\" to \"%s\", anything else to quit: ", title.c_str(), message.c_str(), agreeLabel.c_str());
    char answer[16] = {};
    if (std::fgets(answer, sizeof(answer), stdin) == nullptr) return false;
    return std::strncmp(answer, "yes", 3) == 0;
}

void OS::AttachParentConsole() {}

void* OS::LoadObject(const std::string& path)
{
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void* OS::GetObjectSymbol(void* handle, const std::string& name)
{
    return dlsym(handle, name.c_str());
}

void OS::UnloadObject(void* handle)
{
    dlclose(handle);
}

} // namespace Volcano::Platform

#endif
