#pragma once

#include <functional>
#include <string>
#include <vector>

// Crash reports, so crashes players hit can be fixed.
//
// - Everything the game prints goes to a session log too, and the last few hundred lines are
//   kept in memory.
// - When the game crashes (a bad memory access, an abort, a C++ exception nothing caught),
//   it writes a report into "Crash Reports" in the game's preferences folder: what happened,
//   a backtrace with each module's load address (so addresses can be turned back into code
//   lines with this build's symbols), what the world was doing, and the recent log.
// - The next time the game starts, it offers to show the report.
namespace CrashReport {

// Call first thing in main().
void install();

// Call once a frame. If frames stop for 8 seconds, a "not responding" report is written with
// what the main thread is doing.
void heartbeat();
// Expected native menu/dialog event loops are not simulation hangs. Nestable.
void begin_native_wait();
void end_native_wait();

// A short description of the game's state for reports, filled in by the engine (the world
// file, tick, selected creature, ...). Called from the crash handler, so it must not
// allocate much or take locks.
void set_state_describer(std::function<std::string()> describer);

// The folder reports go into, and the session log.
std::string reports_folder();
std::string session_log_path();

// The recent log lines, oldest first.
std::vector<std::string> recent_log();

// The report left by a crash in the previous session, if it hasn't been seen yet; reading it
// marks it seen.
std::string take_unseen_report(std::string* path = nullptr);

// This build's identification (version and source revision).
const char* build_id();

} // namespace CrashReport
