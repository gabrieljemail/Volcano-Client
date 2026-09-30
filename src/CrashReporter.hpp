#pragma once
#ifndef VOLCANO_CRASH_REPORTER_H
#define VOLCANO_CRASH_REPORTER_H

#include <string>

namespace Volcano {

// Turns silent terminations (uncaught exceptions on any thread, abort(),
// failed asserts, native crashes) into a "the game crashed" dialog.
//
// The crashed process only writes crash-report.txt (reason + stack trace),
// relaunches its own executable with --crash-report to show the dialog, and
// exits immediately — so nothing keeps running behind the dialog. With a
// debugger attached it steps aside and lets the debugger break instead.
class CrashReporter
{
public:
    // Call first thing in main().
    static void Install();

    // Call right after Install(): if this process was launched to show a
    // report, shows it and returns true (main should then exit).
    static bool HandleReportMode(int argc, char** argv);

    [[noreturn]] static void Report(const std::string& reason);
};

} // namespace Volcano

#endif
