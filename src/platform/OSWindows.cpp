#ifdef _WIN32

#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <winhttp.h>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "OS.hpp"

namespace Volcano::Platform {

namespace
{
OS::CrashHandler nativeCrashHandler = nullptr;

// Raw mouse motion not yet handed to ReadBufferedRawMouse.
double rawMouseX = 0.0, rawMouseY = 0.0;
WNDPROC glfwWindowProc = nullptr;

void AccumulateRawMouse(const RAWINPUT& input)
{
    if (input.header.dwType != RIM_TYPEMOUSE) return;
    if (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return; // tablets/remote desktop
    rawMouseX += input.data.mouse.lLastX;
    rawMouseY += input.data.mouse.lLastY;
}

// Catches WM_INPUT that the message loop dispatches before the next batched
// read, so no motion is lost, then hands everything on to GLFW.
LRESULT CALLBACK RawInputWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INPUT)
    {
        RAWINPUT input;
        UINT size = sizeof(input);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1))
        {
            AccumulateRawMouse(input);
        }
    }
    return CallWindowProcW(glfwWindowProc, hwnd, message, wParam, lParam);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info)
{
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "Native crash: exception 0x%08lX at address %p.",
        info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    if (nativeCrashHandler) nativeCrashHandler(buffer);
    return EXCEPTION_CONTINUE_SEARCH;
}

std::string QuoteArgument(const std::string& arg)
{
    std::string quoted = "\"";
    for (char c : arg)
    {
        if (c == '"') quoted += '\\';
        quoted += c;
    }
    return quoted + "\"";
}
}

uint64_t OS::GetProcessMemoryUsageBytes()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
    {
        return static_cast<uint64_t>(counters.PrivateUsage);
    }
    return 0;
}

uint64_t OS::GetAvailablePhysicalMemoryBytes()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? static_cast<uint64_t>(status.ullAvailPhys) : 0;
}

std::string OS::GetCpuModel()
{
    // CPUID brand string: 48 bytes across leaves 0x80000002-0x80000004.
    int brand[12] = {};
    __cpuid(brand, static_cast<int>(0x80000000));
    if (static_cast<unsigned int>(brand[0]) < 0x80000004u) return "";

    __cpuid(brand + 0, static_cast<int>(0x80000002));
    __cpuid(brand + 4, static_cast<int>(0x80000003));
    __cpuid(brand + 8, static_cast<int>(0x80000004));
    return std::string(reinterpret_cast<char*>(brand), sizeof(brand));
}

std::string OS::GetExecutablePath()
{
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    return std::string(path, length);
}

std::vector<std::string> OS::GetFallbackFontPaths()
{
    char windowsDir[MAX_PATH];
    UINT length = GetWindowsDirectoryA(windowsDir, MAX_PATH);
    std::string fonts = std::string(windowsDir, length) + "\\Fonts\\";

    std::vector<std::string> paths;
    for (const char* name : { "segoeui.ttf", "seguisym.ttf", "seguiemj.ttf" })
    {
        std::string path = fonts + name;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) paths.push_back(path);
    }
    return paths;
}

bool OS::IsDebuggerAttached()
{
    return IsDebuggerPresent() != FALSE;
}

bool OS::SetAboveNormalPriority()
{
    return SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != FALSE;
}

void OS::PreciseSleep(int64_t microseconds)
{
    if (microseconds <= 0) return;

    // High-resolution waitable timer (Windows 10 1803+); falls back to a
    // 1 ms-period Sleep where unavailable.
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -microseconds * 10; // relative, in 100 ns units
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
        {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }

    static bool periodSet = (timeBeginPeriod(1), true);
    (void)periodSet;
    Sleep(static_cast<DWORD>(microseconds / 1000));
}

std::string OS::CaptureStackTrace(int skipFrames)
{
    HANDLE process = GetCurrentProcess();
    static bool symbolsReady = false;
    if (!symbolsReady)
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        symbolsReady = SymInitialize(process, nullptr, TRUE) != FALSE;
    }

    void* frames[48];
    USHORT count = CaptureStackBackTrace(static_cast<DWORD>(skipFrames + 1), 48, frames, nullptr);

    alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);

    std::string trace;
    for (USHORT i = 0; i < count; i++)
    {
        DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
        char line[512];

        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (symbolsReady && SymFromAddr(process, address, &displacement, symbol))
        {
            IMAGEHLP_LINE64 source{};
            source.SizeOfStruct = sizeof(source);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddr64(process, address, &lineDisplacement, &source))
            {
                std::snprintf(line, sizeof(line), "  %s (%s:%lu)\n", symbol->Name, source.FileName, source.LineNumber);
            }
            else
            {
                std::snprintf(line, sizeof(line), "  %s + 0x%llx\n", symbol->Name, displacement);
            }
        }
        else
        {
            std::snprintf(line, sizeof(line), "  0x%llx\n", address);
        }
        trace += line;
    }
    return trace;
}

void OS::InstallNativeCrashHandler(CrashHandler handler)
{
    nativeCrashHandler = handler;
    SetUnhandledExceptionFilter(OnUnhandledException);

    // Failed asserts print to stderr instead of a CRT dialog (other threads
    // keep running behind that dialog), and abort() skips the WER dump.
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

bool OS::LaunchDetached(const std::string& executable, const std::vector<std::string>& args)
{
    std::string commandLine = QuoteArgument(executable);
    for (const std::string& arg : args) commandLine += " " + QuoteArgument(arg);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
            DETACHED_PROCESS, nullptr, nullptr, &startup, &info))
    {
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

void OS::ShowErrorDialog(const std::string& title, const std::string& message)
{
    MessageBoxA(nullptr, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}

bool OS::EnableBufferedRawMouse(GLFWwindow* window)
{
    HWND hwnd = glfwGetWin32Window(window);
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01; // HID generic desktop
    device.usUsage = 0x02;     // mouse
    device.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) return false;

    glfwWindowProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RawInputWindowProc)));
    return glfwWindowProc != nullptr;
}

void OS::ReadBufferedRawMouse(double& dx, double& dy)
{
    // One GetRawInputBuffer call drains a whole batch of queued mouse
    // reports, instead of one WM_INPUT round trip per report.
    alignas(8) static BYTE buffer[16 * 1024];
    for (;;)
    {
        UINT size = sizeof(buffer);
        UINT count = GetRawInputBuffer(reinterpret_cast<RAWINPUT*>(buffer), &size, sizeof(RAWINPUTHEADER));
        if (count == 0 || count == static_cast<UINT>(-1)) break;

        RAWINPUT* input = reinterpret_cast<RAWINPUT*>(buffer);
        for (UINT i = 0; i < count; i++)
        {
            AccumulateRawMouse(*input);
            // NEXTRAWINPUTBLOCK, spelled out: the SDK macro uses an undefined QWORD.
            ULONG_PTR next = reinterpret_cast<ULONG_PTR>(input) + input->header.dwSize;
            input = reinterpret_cast<RAWINPUT*>((next + 7) & ~static_cast<ULONG_PTR>(7));
        }
    }

    dx += rawMouseX;
    dy += rawMouseY;
    rawMouseX = 0.0;
    rawMouseY = 0.0;
}

const char* OS::GetObjectExtension()
{
    return ".dll";
}

namespace
{
std::wstring ToWide(const std::string& text)
{
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

struct WinHttpHandle
{
    HINTERNET handle = nullptr;
    explicit WinHttpHandle(HINTERNET h) : handle(h) {}
    ~WinHttpHandle() { if (handle != nullptr) WinHttpCloseHandle(handle); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};
} // namespace

bool OS::HttpGet(const std::string& url, std::vector<uint8_t>& body, std::string& error,
    const std::function<void(uint64_t, uint64_t)>& progress)
{
    body.clear();
    std::wstring wideUrl = ToWide(url);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
    {
        error = "Not a valid https address: " + url;
        return false;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength + parts.dwExtraInfoLength);

    WinHttpHandle session(WinHttpOpen(L"VolcanoClient", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
    if (session.handle == nullptr)
    {
        error = "Couldn't start the network session.";
        return false;
    }
    WinHttpHandle connection(WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0));
    if (connection.handle == nullptr)
    {
        error = "Couldn't connect to the server.";
        return false;
    }
    WinHttpHandle request(WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (request.handle == nullptr
        || !WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request.handle, nullptr))
    {
        error = "The request failed (error " + std::to_string(GetLastError()) + ").";
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
    {
        error = "The server answered " + std::to_string(status) + ".";
        return false;
    }

    DWORD length = 0;
    DWORD lengthSize = sizeof(length);
    uint64_t total = 0;
    if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &length, &lengthSize, WINHTTP_NO_HEADER_INDEX))
    {
        total = length;
        body.reserve(total);
    }

    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available))
        {
            error = "The download was interrupted.";
            return false;
        }
        if (available == 0) break;

        size_t at = body.size();
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, body.data() + at, available, &read))
        {
            error = "The download was interrupted.";
            return false;
        }
        body.resize(at + read);
        if (progress) progress(body.size(), total);
    }
    return true;
}

bool OS::ShowConsentDialog(const std::string& title, const std::string& message, const std::string& agreeLabel)
{
    std::string text = message + "\n\nChoose Yes to \"" + agreeLabel + "\", or No to quit.";
    int answer = MessageBoxW(nullptr, ToWide(text).c_str(), ToWide(title).c_str(), MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
    return answer == IDYES;
}

void OS::AttachParentConsole()
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return; // Redirected or already attached.
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(console, &mode)) SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING); // The log colors.
}

void* OS::LoadObject(const std::string& path)
{
    return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
}

void* OS::GetObjectSymbol(void* handle, const std::string& name)
{
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name.c_str()));
}

void OS::UnloadObject(void* handle)
{
    FreeLibrary(static_cast<HMODULE>(handle));
}

} // namespace Volcano::Platform

#endif
