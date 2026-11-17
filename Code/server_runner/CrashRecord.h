#pragma once

// A server that dies must say where and why (work plan 4.5).
//
// Until 4.1.1 the dedicated server had no crash handling at all: an access violation or an exception
// nobody caught closed the console window, and logs/STServerOut.log simply ended. The host of a coop
// session could only report "the server closed".
//
// Install() arranges for two kinds of record in logs/STServer_Crash.log (append-only; the report
// collector takes the whole logs folder):
//
//   [UNHANDLED_CRASH] ...   an unhandled structured exception (access violation, stack overflow, a
//                           C++ throw nobody catches): code, address, module, and the call stack as
//                           module+offset. Same layout as the client's record, so
//                           Tools/rfab/symbolize_crash.py names the functions with
//                           RFAB_Online_Server_Symbols.gz. Windows only.
//   [TERMINATE] ...         std::terminate was called: an exception that escaped main, a thread or a
//                           noexcept function. Carries the exception's what() text.
//
// Both are written with the operating system's file calls into a static buffer: no heap, no CRT
// streams, no logger - any of those may be the thing that is broken.
namespace ServerCrash
{
// Call first in main(). Safe to call once; never throws.
void Install() noexcept;

// The switch `--rfab-crash-self-test fault|terminate` of the server executable. The release pipeline
// runs it and then reads logs/STServer_Crash.log: this is how every build proves that a dying
// server leaves a record that the symbol table of the same build can name.
//
//   fault      calls apFault, which must write through a null pointer inside the server library
//   terminate  lets an exception escape a noexcept function
//
// Never returns normally in either mode: the process ends itself as soon as the record is written
// (exit code kSelfTestFaultExit / kSelfTestTerminateExit), without the system's crash dialogs.
// Returns 2 for an unknown mode.
inline constexpr int kSelfTestFaultExit = 70;
inline constexpr int kSelfTestTerminateExit = 71;
inline constexpr char kSelfTestSwitch[] = "--rfab-crash-self-test";
inline constexpr char kSelfTestTerminateText[] = "RFAB crash self-test: this text must reach the crash log";
int RunSelfTest(const char* apMode, void (*apFault)()) noexcept;
} // namespace ServerCrash
