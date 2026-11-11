#pragma once

#include <cstddef>

// The crash report: when the program itself crashes (an access violation, an illegal instruction, a stack overflow, an abort, an
// exception nobody caught) it writes a few lines to the log (standard error) that say what happened, in which thread, where in the
// program (module + offset and the function name read from the program's own symbol table, so the lines make sense on the tester's
// PC), whether that is game code the live compilers made, and where the game was. Then it calls `after` and lets the system end the
// program as it would have. Costs nothing while nothing crashes.
namespace crash {

// Called once after the lines are written, on the thread that crashed or on the report's helper thread (Windows). `crashed` is false
// for a closed terminal (SIGHUP), true for everything else; `report` holds the lines written (for a log copy that may be stuck).
using After = void (*)(bool crashed, const char *report, std::size_t size);

// Installs the handlers (Windows: the unhandled exception filter, SIGABRT, std::terminate and the C runtime's invalid-parameter handler;
// elsewhere: SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT and SIGHUP, and std::terminate). Call it first thing in main().
void install(After after);

}
