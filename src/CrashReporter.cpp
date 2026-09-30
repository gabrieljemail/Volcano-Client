#include "CrashReporter.hpp"
#include "platform/OS.hpp"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>

namespace Volcano {

namespace
{
constexpr const char* REPORT_FLAG = "--crash-report";
std::atomic<bool> reporting{false};

void OnTerminate()
{
    std::string reason = "Unknown fatal error (std::terminate).";
    if (std::exception_ptr current = std::current_exception())
    {
        try
        {
            std::rethrow_exception(current);
        }
        catch (const std::exception& e)
        {
            reason = std::string("Unhandled exception: ") + e.what();
        }
        catch (...)
        {
            reason = "Unhandled non-standard exception.";
        }
    }
    CrashReporter::Report(reason);
}

void OnAbort(int)
{
    CrashReporter::Report("abort() was called, usually by a failed assertion (see the console).");
}

void OnNativeCrash(const std::string& reason)
{
    CrashReporter::Report(reason);
}

std::string ReportPath()
{
    std::string exe = Platform::OS::GetExecutablePath();
    size_t slash = exe.find_last_of("/\\");
    return (slash == std::string::npos ? std::string(".") : exe.substr(0, slash)) + "/crash-report.txt";
}
}

void CrashReporter::Install()
{
    std::set_terminate(OnTerminate);
    std::signal(SIGABRT, OnAbort);
    Platform::OS::InstallNativeCrashHandler(OnNativeCrash);
}

bool CrashReporter::HandleReportMode(int argc, char** argv)
{
    if (argc < 3 || std::strcmp(argv[1], REPORT_FLAG) != 0) return false;

    std::ifstream file(argv[2]);
    std::stringstream report;
    report << file.rdbuf();
    Platform::OS::ShowErrorDialog("Volcano Client crashed",
        "Volcano Client crashed and had to close.\n\n" + report.str()
        + "\nThis report was saved to:\n" + argv[2]);
    return true;
}

void CrashReporter::Report(const std::string& reason)
{
    // Only the first crashing thread reports; any others just exit.
    if (reporting.exchange(true)) std::_Exit(3);

    std::string trace = Platform::OS::CaptureStackTrace(1);
    std::fprintf(stderr, "[FATAL] %s\nStack trace:\n%s", reason.c_str(), trace.c_str());
    std::fflush(stderr);

    // Let an attached debugger break at the real crash site instead.
    if (Platform::OS::IsDebuggerAttached())
    {
        std::signal(SIGABRT, SIG_DFL);
        std::abort();
    }

    std::string path = ReportPath();
    {
        std::ofstream file(path);
        file << reason << "\n\nStack trace:\n" << trace;
    }
    if (!Platform::OS::LaunchDetached(Platform::OS::GetExecutablePath(), { REPORT_FLAG, path }))
    {
        Platform::OS::ShowErrorDialog("Volcano Client crashed", reason);
    }
    std::_Exit(3);
}

} // namespace Volcano
