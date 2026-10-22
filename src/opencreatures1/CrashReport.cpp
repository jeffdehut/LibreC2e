#include "common/SessionProfile.h"
#include "CrashReport.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
// after windows.h
#include <dbghelp.h>
#include <fcntl.h>
#include <io.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>
#ifdef __GNUG__
#include <cxxabi.h>
#endif
#endif

#ifndef OPENC1_BUILD_ID
#define OPENC1_BUILD_ID "unknown"
#endif

namespace fs = std::filesystem;

namespace CrashReport {

namespace {

constexpr size_t RECENT_LINES = 400;

std::mutex s_log_mutex;
std::deque<std::string> s_recent;
std::string s_partial;
std::FILE* s_session_log = nullptr;
int s_original_stdout = -1;
int s_pipe_read = -1;
std::function<std::string()> s_describer;
std::string s_folder;
std::string s_session_path;
std::atomic<bool> s_crashing{false};

std::string pref_folder() {
	if (auto profile = session_profile(); !profile.empty()) { return profile.string(); }
	char* dir = SDL_GetPrefPath("openc2e", "opencreatures1");
	std::string path = dir ? dir : "";
	SDL_free(dir);
	return path;
}

// The game can run for weeks, so the session log is kept to about LOG_LIMIT bytes: when it's
// full it becomes "last session (earlier part).log", replacing the one before, and a new
// part starts.
constexpr long LOG_LIMIT = 8 * 1024 * 1024;
long s_log_size = 0;

void add_output(const char* data, size_t size) {
	std::lock_guard<std::mutex> lock(s_log_mutex);
	if (s_session_log) {
		std::fwrite(data, 1, size, s_session_log);
		std::fflush(s_session_log);
		s_log_size += static_cast<long>(size);
		if (s_log_size > LOG_LIMIT && !s_crashing) {
			std::fclose(s_session_log);
			std::error_code err;
			const fs::path path = s_session_path;
			fs::rename(path, path.parent_path() / "last session (earlier part).log", err);
			s_session_log = std::fopen(s_session_path.c_str(), "w");
			s_log_size = 0;
			if (s_session_log) {
				const std::string note = std::string("* opencreatures1 ") + OPENC1_BUILD_ID + ": the log continues from \"last session (earlier part).log\"\n";
				std::fwrite(note.data(), 1, note.size(), s_session_log);
			}
		}
	}
	for (size_t i = 0; i < size; ++i) {
		if (data[i] == '\n') {
			s_recent.push_back(s_partial);
			s_partial.clear();
			if (s_recent.size() > RECENT_LINES) {
				s_recent.pop_front();
			}
		} else if (data[i] != '\r') {
			s_partial += data[i];
		}
	}
}

// Copies everything written to stdout and stderr into the session log and the recent lines,
// and on to wherever stdout went before (the terminal, or the Mac launcher's log).
void tee_output() {
#ifdef _WIN32
	// a windowed program has no console: give stdout and stderr somewhere to go first
	if (_fileno(stdout) < 0) {
		std::freopen("NUL", "w", stdout);
	}
	if (_fileno(stderr) < 0) {
		std::freopen("NUL", "w", stderr);
	}
	int fds[2];
	if (_pipe(fds, 1 << 16, _O_BINARY) != 0) {
		return;
	}
	s_original_stdout = _dup(_fileno(stdout));
	_dup2(fds[1], _fileno(stdout));
	_dup2(fds[1], _fileno(stderr));
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::setvbuf(stderr, nullptr, _IONBF, 0);
	s_pipe_read = fds[0];
	std::thread([] {
		char buffer[4096];
		int n;
		while ((n = _read(s_pipe_read, buffer, sizeof(buffer))) > 0) {
			if (s_original_stdout >= 0) {
				_write(s_original_stdout, buffer, static_cast<unsigned>(n));
			}
			add_output(buffer, static_cast<size_t>(n));
		}
	}).detach();
#else
	int fds[2];
	if (pipe(fds) != 0) {
		return;
	}
	s_original_stdout = dup(STDOUT_FILENO);
	dup2(fds[1], STDOUT_FILENO);
	dup2(fds[1], STDERR_FILENO);
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	std::setvbuf(stderr, nullptr, _IONBF, 0);
	s_pipe_read = fds[0];
	std::thread([] {
		char buffer[4096];
		ssize_t n;
		while ((n = read(s_pipe_read, buffer, sizeof(buffer))) > 0) {
			if (s_original_stdout >= 0) {
				ssize_t ignored = write(s_original_stdout, buffer, static_cast<size_t>(n));
				(void)ignored;
			}
			add_output(buffer, static_cast<size_t>(n));
		}
	}).detach();
#endif
}

std::string timestamp(const char* format) {
	std::time_t t = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char text[64];
	std::strftime(text, sizeof(text), format, &tm);
	return text;
}

const char* platform_name() {
#if defined(_WIN32)
	return "Windows";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
	return "iOS";
#elif defined(__APPLE__)
	return "macOS";
#elif defined(__linux__)
	return "Linux";
#else
	return "unknown";
#endif
}

#ifdef _WIN32
std::string backtrace_text(CONTEXT* context, HANDLE thread = GetCurrentThread()) {
	std::ostringstream out;
	HANDLE process = GetCurrentProcess();
	SymInitialize(process, NULL, TRUE);
	SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
	CONTEXT record = *context;
	STACKFRAME64 frame{};
	frame.AddrPC.Offset = record.Rip;
	frame.AddrFrame.Offset = record.Rbp;
	frame.AddrStack.Offset = record.Rsp;
	frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
	for (int i = 0; i < 64 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &record, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL); ++i) {
		const DWORD64 address = frame.AddrPC.Offset;
		const DWORD64 base = SymGetModuleBase64(process, address);
		char module[MAX_PATH] = "?";
		if (base) {
			GetModuleFileNameA(reinterpret_cast<HMODULE>(base), module, MAX_PATH);
		}
		const char* name = std::strrchr(module, '\\') ? std::strrchr(module, '\\') + 1 : module;
		char buffer[sizeof(SYMBOL_INFO) + 256];
		auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = 255;
		DWORD64 displacement = 0;
		char line[512];
		std::snprintf(line, sizeof(line), "  #%-2d %s +0x%llx", i, name, static_cast<unsigned long long>(address - base));
		out << line;
		if (SymFromAddr(process, address, &displacement, symbol)) {
			out << "  " << symbol->Name << " +" << displacement;
		}
		out << "\n";
	}
	return out.str();
}
#else
std::string format_frames(void** frames, int count) {
	std::ostringstream out;
	for (int i = 0; i < count; ++i) {
		Dl_info info{};
		dladdr(frames[i], &info);
		const char* module = info.dli_fname ? (std::strrchr(info.dli_fname, '/') ? std::strrchr(info.dli_fname, '/') + 1 : info.dli_fname) : "?";
		char line[512];
		std::snprintf(line, sizeof(line), "  #%-2d %s +0x%lx", i, module, static_cast<unsigned long>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase)));
		out << line;
		if (info.dli_sname) {
			std::string name = info.dli_sname;
#ifdef __GNUG__
			int status = 0;
			if (char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status)) {
				if (status == 0) {
					name = demangled;
				}
				std::free(demangled);
			}
#endif
			out << "  " << name << " +" << (reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_saddr));
		}
		out << "\n";
	}
	return out.str();
}

std::string backtrace_text() {
	void* frames[64];
	const int count = backtrace(frames, 64);
	return format_frames(frames, count);
}
#endif

// Writes the report; returns its path.
std::string write_report(const std::string& reason, const std::string& backtrace, const char* kind = "crash") {
	std::fflush(stdout);
	std::fflush(stderr);
	// give the log thread a moment to catch the last lines
	std::this_thread::sleep_for(std::chrono::milliseconds(150));

	std::error_code err;
	fs::create_directories(s_folder, err);
	const std::string path = (fs::path(s_folder) / (std::string(kind) + " " + timestamp("%Y-%m-%d %H-%M-%S") + ".txt")).string();
	std::ofstream out(path);
	out << "opencreatures1 " << kind << " report\n";
	out << "================================\n\n";
	out << "When:     " << timestamp("%Y-%m-%d %H:%M:%S") << "\n";
	out << "Build:    " << build_id() << "\n";
	out << "Platform: " << platform_name() << "\n";
	out << "What:     " << reason << "\n\n";
	out << "Backtrace of the game's main thread (module +offset from where it's loaded; most recent call first):\n";
	out << backtrace << "\n";
	if (s_describer) {
		out << "The game:\n";
		try {
			out << s_describer() << "\n";
		} catch (...) {
			out << "  (couldn't describe the game's state)\n";
		}
	}
	out << "The last lines of the log:\n";
	{
		std::unique_lock<std::mutex> lock(s_log_mutex, std::try_to_lock);
		for (const auto& line : s_recent) {
			out << "  " << line << "\n";
		}
		if (!s_partial.empty()) {
			out << "  " << s_partial << "\n";
		}
	}
	out << "\nThe whole log of this session: " << s_session_path << "\n";
	out.close();
	// remember it, so the next start can show it
	std::ofstream(fs::path(s_folder) / "unseen.txt") << path;
	return path;
}

void announce(const std::string& path) {
	const std::string text = "Creatures crashed, sorry! A crash report was saved:\n\n" + path + "\n\nPlease send it along so the problem can be fixed.";
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Creatures", text.c_str(), nullptr);
}

// --- the watchdog: a report when the game stops responding ---

constexpr auto HANG_SECONDS = std::chrono::seconds(8);
std::atomic<int64_t> s_heartbeat_ms{0};
std::atomic<bool> s_watchdog_running{false};
std::atomic<unsigned> s_native_waits{0};

int64_t now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
HANDLE s_main_thread = nullptr;

std::string main_thread_backtrace() {
	if (!s_main_thread || SuspendThread(s_main_thread) == static_cast<DWORD>(-1)) {
		return "  (couldn't stop the main thread to look at it)\n";
	}
	CONTEXT context{};
	context.ContextFlags = CONTEXT_FULL;
	std::string text = GetThreadContext(s_main_thread, &context) ? backtrace_text(&context, s_main_thread) : "  (couldn't read the main thread)\n";
	ResumeThread(s_main_thread);
	return text;
}
#else
pthread_t s_main_thread;
void* s_hang_frames[64];
std::atomic<int> s_hang_frame_count{-1};

// runs on the main thread, wherever it's stuck: only records the return addresses
void on_hang_signal(int) {
	s_hang_frame_count = backtrace(s_hang_frames, 64);
}

std::string main_thread_backtrace() {
	s_hang_frame_count = -1;
	pthread_kill(s_main_thread, SIGUSR1);
	for (int i = 0; i < 100 && s_hang_frame_count < 0; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (s_hang_frame_count < 0) {
		return "  (the main thread didn't answer)\n";
	}
	return format_frames(s_hang_frames, s_hang_frame_count);
}
#endif

void watchdog() {
	bool reported = false;
	while (s_watchdog_running) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const int64_t last = s_heartbeat_ms;
		if (last == 0 || s_native_waits.load() != 0) {
			continue; // not started yet
		}
		const bool stuck = now_ms() - last > std::chrono::duration_cast<std::chrono::milliseconds>(HANG_SECONDS).count();
		if (stuck && !reported && !s_crashing) {
			reported = true;
			std::printf("* The game hasn't responded for %lld seconds; writing a report\n", static_cast<long long>(HANG_SECONDS.count()));
			write_report("the game stopped responding (no frame for 8 seconds)", main_thread_backtrace(), "not responding");
		} else if (!stuck && reported) {
			reported = false;
			std::printf("* The game is responding again\n");
		}
	}
}

const char* signal_name(int sig) {
	switch (sig) {
		case SIGSEGV: return "SIGSEGV (a bad memory access)";
		case SIGABRT: return "SIGABRT (the program aborted)";
		case SIGFPE: return "SIGFPE (a maths error, such as dividing by zero)";
		case SIGILL: return "SIGILL (an illegal instruction)";
#ifndef _WIN32
		case SIGBUS: return "SIGBUS (a bad memory access)";
		case SIGTRAP: return "SIGTRAP";
#endif
	}
	return "an unknown signal";
}

#ifdef _WIN32
LONG WINAPI on_windows_exception(EXCEPTION_POINTERS* info) {
	if (s_crashing.exchange(true)) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	char reason[128];
	std::snprintf(reason, sizeof(reason), "Windows exception 0x%08lx at 0x%p", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
	announce(write_report(reason, backtrace_text(info->ContextRecord)));
	return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void on_signal(int sig) {
	if (s_crashing.exchange(true)) {
		std::_Exit(1);
	}
#ifdef _WIN32
	CONTEXT context;
	RtlCaptureContext(&context);
	announce(write_report(signal_name(sig), backtrace_text(&context)));
#else
	announce(write_report(signal_name(sig), backtrace_text()));
#endif
	std::_Exit(1);
}

void on_terminate() {
	std::string reason = "a C++ exception that nothing caught";
	if (auto e = std::current_exception()) {
		try {
			std::rethrow_exception(e);
		} catch (const std::exception& ex) {
			reason += std::string(": ") + ex.what();
		} catch (...) {
		}
	}
	if (!s_crashing.exchange(true)) {
#ifdef _WIN32
		CONTEXT context;
		RtlCaptureContext(&context);
		announce(write_report(reason, backtrace_text(&context)));
#else
		announce(write_report(reason, backtrace_text()));
#endif
	}
	std::_Exit(1);
}

} // namespace

const char* build_id() {
	return OPENC1_BUILD_ID;
}

#if defined(__APPLE__) && TARGET_OS_OSX
extern "C" void oc1_mac_watch_menu_tracking(void (*changed)(int));
#endif

void install() {
#if defined(__APPLE__) && TARGET_OS_OSX
	oc1_mac_watch_menu_tracking([](int began) {
		if (began) begin_native_wait(); else end_native_wait();
	});
#endif
	const std::string pref = pref_folder();
	s_folder = (fs::path(pref) / "Crash Reports").string();
	std::error_code err;
	fs::create_directories(fs::path(pref) / "Logs", err);
	s_session_path = (fs::path(pref) / "Logs" / "last session.log").string();
	// keep the session before last too
	fs::rename(s_session_path, (fs::path(pref) / "Logs" / "session before last.log"), err);
	s_session_log = std::fopen(s_session_path.c_str(), "w");
	tee_output();
	std::printf("* opencreatures1 %s on %s\n", build_id(), platform_name());

	std::set_terminate(on_terminate);
	// Diagnostic builds can leave fatal signals to the sanitizer and OS. Our handler
	// exits after writing its report, which otherwise suppresses native crash diagnostics.
	if (!std::getenv("OPENCREATURES1_NATIVE_CRASH_REPORTS")) {
		std::signal(SIGSEGV, on_signal);
		std::signal(SIGABRT, on_signal);
		std::signal(SIGFPE, on_signal);
		std::signal(SIGILL, on_signal);
#ifndef _WIN32
		std::signal(SIGBUS, on_signal);
		// failed library hardening checks (e.g. an index out of range) trap on arm64
		std::signal(SIGTRAP, on_signal);
#endif
	}
#ifdef _WIN32
	SetUnhandledExceptionFilter(on_windows_exception);
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
#else
	s_main_thread = pthread_self();
	std::signal(SIGUSR1, on_hang_signal);
#endif
	s_watchdog_running = true;
	std::thread(watchdog).detach();
}

void begin_native_wait() { ++s_native_waits; }
void end_native_wait() {
	heartbeat();
	if (s_native_waits.load()) --s_native_waits;
}

void heartbeat() {
	s_heartbeat_ms = now_ms();
}

void set_state_describer(std::function<std::string()> describer) {
	s_describer = std::move(describer);
}

std::string reports_folder() {
	return s_folder;
}

std::string session_log_path() {
	return s_session_path;
}

std::vector<std::string> recent_log() {
	std::lock_guard<std::mutex> lock(s_log_mutex);
	return {s_recent.begin(), s_recent.end()};
}

std::string take_unseen_report(std::string* path_out) {
	std::error_code err;
	const auto marker = fs::path(s_folder) / "unseen.txt";
	std::ifstream in(marker);
	std::string path;
	std::getline(in, path);
	in.close();
	fs::remove(marker, err);
	if (path.empty()) {
		return "";
	}
	std::ifstream report(path);
	std::stringstream text;
	text << report.rdbuf();
	if (path_out) {
		*path_out = path;
	}
	return text.str();
}

} // namespace CrashReport
