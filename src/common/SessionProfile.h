#pragma once
#include <cstdlib>
#include <filesystem>
// Standalone runtimes retain their legacy preferences. The shared entry point supplies
// a profile belonging exclusively to its copied installation and selected world.
inline std::filesystem::path session_profile() {
	const char* path = std::getenv("OPENC2E_SESSION_PROFILE");
	return path && *path ? std::filesystem::path(path) : std::filesystem::path{};
}
