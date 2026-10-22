#pragma once
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
// Scope a deterministic buffered-write failure to this test process. Restore the
// previous soft limit and signal handler before any subsequent test runs.
class FileWriteFailure {
	rlimit previous{};
	void (*handler)(int) = SIG_DFL;
	bool active = false;
public:
	FileWriteFailure() {
		if (getrlimit(RLIMIT_FSIZE, &previous) != 0) return;
		auto limited = previous;
		limited.rlim_cur = 0;
		handler = std::signal(SIGXFSZ, SIG_IGN);
		active = setrlimit(RLIMIT_FSIZE, &limited) == 0;
		if (!active) std::signal(SIGXFSZ, handler);
	}
	explicit operator bool() const { return active; }
	~FileWriteFailure() {
		if (active) { setrlimit(RLIMIT_FSIZE, &previous); std::signal(SIGXFSZ, handler); }
	}
};
#endif
