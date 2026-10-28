#include "LocalServer.hpp"
#include "../CrashReporter.hpp"
#include "../Logger.hpp"
#include <chrono>
#include <cstring>
#include <thread>

namespace Volcano {

bool LocalServer::WaitUntilReady(std::stop_token stopToken)
{
    while (true)
    {
        if (stopToken.stop_requested()) return false;
        if (!IsRunning())
        {
            Log::Error("LocalServer: process exited before becoming ready");
            return false;
        }
        if (serverReadyForLogin.load()) return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

#ifdef _WIN32

LocalServer::~LocalServer() { Stop(); }

bool LocalServer::Start(const std::string& directory, const std::string& javaPath,
    const std::string& javaArguments, uint16_t port)
{
    // Anonymous pipe for the child's stdin, so Stop() can send a graceful
    // "stop" console command the same way a real server operator would,
    // instead of always hard-killing the process.
    SECURITY_ATTRIBUTES pipeAttributes{};
    pipeAttributes.nLength = sizeof(pipeAttributes);
    pipeAttributes.bInheritHandle = TRUE;

    HANDLE stdinRead = nullptr;
    if (!CreatePipe(&stdinRead, &stdinWrite, &pipeAttributes, 0))
    {
        Log::Error("LocalServer: failed to create stdin pipe");
        return false;
    }
    // Only the read end should be inherited by the child
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);

    // Same story for stdout/stderr, in the other direction: one pipe, both
    // ends inheritable-write-side-only, so DrainOutputPipe (started below,
    // once the process actually exists) can log the server's own console
    // output with a [SERVER] prefix instead of it going nowhere
    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &pipeAttributes, 0))
    {
        Log::Error("LocalServer: failed to create stdout pipe");
        CloseHandle(stdinRead);
        CloseHandle(stdinWrite);
        stdinWrite = nullptr;
        return false;
    }
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = stdinRead;
    // Both stdout and stderr point at the same pipe
    startupInfo.hStdOutput = stdoutWrite;
    startupInfo.hStdError = stdoutWrite;

    // javaw.exe (the JavaPath this project's settings.json expects) is
    // Java's own windowless launcher
    std::string commandLine = "\"" + javaPath + "\" " + javaArguments + " -jar server.jar"
        + " --port " + std::to_string(port) + " --nogui";

    PROCESS_INFORMATION processInfo{};
    // lpCommandLine must be a writable buffer per CreateProcess's own
    // contract (it may modify it in place)
    BOOL created = CreateProcessA(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        /* bInheritHandles */ TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        directory.c_str(),
        &startupInfo,
        &processInfo);

    // Our copies
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);

    if (!created)
    {
        Log::Error("LocalServer: failed to launch '" + commandLine + "' in " + directory);
        CloseHandle(stdinWrite);
        stdinWrite = nullptr;
        CloseHandle(stdoutRead);
        stdoutRead = nullptr;
        return false;
    }

    processHandle = processInfo.hProcess;
    CloseHandle(processInfo.hThread); // Never used past creation.

    // See jobHandle's own comment. Best-effort: a failure here just means
    // the orphan-on-crash protection doesn't apply this session (logged,
    // not fatal) - the server itself is already up and shouldn't be killed
    // over a safety net failing to attach.
    jobHandle = CreateJobObjectA(nullptr, nullptr);
    if (jobHandle != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(jobHandle, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        if (!AssignProcessToJobObject(jobHandle, processHandle))
        {
            Log::Error("LocalServer: failed to assign server process to job object - it may survive a client crash");
            CloseHandle(jobHandle);
            jobHandle = nullptr;
        }
    }
    else
    {
        Log::Error("LocalServer: failed to create job object - server may survive a client crash");
    }

    outputReaderThread = std::thread([this] { CrashReporter::InstallForThread(); DrainOutputPipe(); });
    return true;
}

void LocalServer::DrainOutputPipe()
{
    std::string lineBuffer;
    char chunk[4096];
    DWORD bytesRead = 0;
    // Blocks here between reads for as long as the pipe's write end (the
    // server process) is alive - this returns false/0 once the process
    // exits and closes its copy, which is what ends this loop.
    while (ReadFile(stdoutRead, chunk, sizeof(chunk), &bytesRead, nullptr) && bytesRead > 0)
    {
        lineBuffer.append(chunk, bytesRead);
        size_t newline;
        while ((newline = lineBuffer.find('\n')) != std::string::npos)
        {
            std::string line = lineBuffer.substr(0, newline);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            Log::Info("" + line);
            if (onOutputLine) onOutputLine(line);
            // Vanilla's own "fully started, safe to log in" line
            if (line.find("Done (") != std::string::npos) serverReadyForLogin.store(true);
            lineBuffer.erase(0, newline + 1);
        }
    }
    if (!lineBuffer.empty())
    {
        // Trailing partial line, no final newline.
        Log::Info("" + lineBuffer);
        if (onOutputLine) onOutputLine(lineBuffer);
        if (lineBuffer.find("Done (") != std::string::npos) serverReadyForLogin.store(true);
    }

    CloseHandle(stdoutRead);
    stdoutRead = nullptr;
}

bool LocalServer::IsRunning() const
{
    if (processHandle == nullptr) return false;
    DWORD exitCode = 0;
    return GetExitCodeProcess(processHandle, &exitCode) && exitCode == STILL_ACTIVE;
}

void LocalServer::SendCommand(const std::string& command)
{
    if (!IsRunning() || stdinWrite == nullptr) return;

    std::string line = command + "\r\n";
    DWORD written = 0;
    WriteFile(stdinWrite, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

void LocalServer::Stop()
{
    if (processHandle == nullptr) return;

    SendCommand("stop");
    if (stdinWrite != nullptr)
    {
        CloseHandle(stdinWrite);
        stdinWrite = nullptr;
    }

    // Give the server a real chance at a clean shutdown (saving the world,
    // closing region files) before falling back to a hard kill.
    if (WaitForSingleObject(processHandle, 30000) != WAIT_OBJECT_0)
    {
        Log::Error("LocalServer: didn't stop gracefully in time, terminating");
        TerminateProcess(processHandle, 1);
        WaitForSingleObject(processHandle, 5000);
    }

    // The process is gone either way by this point, so its copies of the
    // stdout/stderr pipe are closed too
    if (outputReaderThread.joinable()) outputReaderThread.join();

    CloseHandle(processHandle);
    processHandle = nullptr;

    // Only closed here, after the process is already confirmed dead above -
    // closing it any earlier, while the process might still be running,
    // would trigger JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE and hard-kill a
    // server this function is still trying to shut down gracefully.
    if (jobHandle != nullptr)
    {
        CloseHandle(jobHandle);
        jobHandle = nullptr;
    }
}

#else

LocalServer::~LocalServer() = default;

bool LocalServer::Start(const std::string&, const std::string&, const std::string&, uint16_t)
{
    Log::Error("LocalServer: launching a local server isn't implemented on this platform yet");
    return false;
}

bool LocalServer::IsRunning() const { return false; }

void LocalServer::SendCommand(const std::string&) {}

void LocalServer::Stop() {}

#endif

} // namespace Volcano
