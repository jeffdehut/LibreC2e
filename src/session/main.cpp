#include "Session.h"
#include <SDL3/SDL.h>
#include <cxxopts.hpp>
#include <fmt/core.h>
#include <chrono>
#include <atomic>
#include <memory>
#include <fstream>
#include <stdexcept>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
namespace fs = std::filesystem;
static fs::path runtime_directory() {
#ifdef __APPLE__
	// SDL_GetBasePath points at Contents/Resources in a bundle. Runtime helpers
	// belong beside the executable in Contents/MacOS.
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> executable(size);
	if (_NSGetExecutablePath(executable.data(), &size) != 0) throw std::runtime_error("Unable to locate application executable");
	return fs::canonical(executable.data()).parent_path();
#else
	const char* base = SDL_GetBasePath();
	if (!base) throw std::runtime_error(SDL_GetError());
	return fs::path(base);
#endif
}
static std::string choose(bool folder, const char* location) {
	struct Result { std::atomic<bool> done{false}; std::string path; } result;
	auto callback = [](void* data, const char* const* files, int) {
		auto& r = *static_cast<Result*>(data);
		if (files && files[0]) r.path = files[0];
		r.done = true;
	};
	if (folder) SDL_ShowOpenFolderDialog(callback, &result, nullptr, location, false);
	else {
		static const SDL_DialogFileFilter filter{"Creatures world", "sfc"};
		SDL_ShowOpenFileDialog(callback, &result, nullptr, &filter, 1, location, false);
	}
	while (!result.done) { SDL_PumpEvents(); SDL_Delay(20); }
	return result.path;
}
static int run(const session::Selection& s, const session::Copy& copy, bool no_save, int duration, bool parse_only) {
	auto executable = runtime_directory() / (s.game == session::Game::c1 ? "opencreatures1" : "openc2e-c2");
#ifdef _WIN32
	executable += ".exe";
#endif
	if (!fs::is_regular_file(executable)) throw std::runtime_error("Missing application runtime: " + executable.string());
	auto lock = copy.root / "running.lock";
	if (!fs::create_directory(lock)) throw std::runtime_error("Session is already running, or a previous run ended unexpectedly. Check it has stopped before removing: " + lock.string());
	struct Unlock { fs::path path; ~Unlock() { std::error_code ec; fs::remove(path, ec); } } unlock{lock};
	std::string active_world = copy.world.filename().string();
	if (s.game == session::Game::c1) {
		std::ifstream record(copy.profile / "active-world.txt");
		std::string saved;
		if (std::getline(record, saved)) {
			if (saved.empty() || fs::path(saved).filename() != fs::path(saved) || saved == "." || saved == "..")
				throw std::runtime_error("Invalid active world in session profile");
			if (session::world_game(copy.data / saved) != s.game) throw std::runtime_error("Saved session world has the wrong format");
			active_world = saved;
		}
	}
	std::vector<std::string> arguments{executable.string()};
	if (s.game == session::Game::c1) {
		if (parse_only) throw std::runtime_error("--parse-only currently supports C2; C1 has separate SFC round-trip tests");
		if (no_save) arguments.push_back("--no-save");
		arguments.push_back(copy.data.string());
	} else {
		arguments.insert(arguments.end(), {"--data-path", copy.data.string(), "--bootstrap", copy.world.string()});
		if (parse_only) arguments.push_back("--norun");
	}
	std::vector<const char*> args;
	for (auto& arg : arguments) args.push_back(arg.c_str());
	args.push_back(nullptr);
	std::unique_ptr<SDL_Environment, decltype(&SDL_DestroyEnvironment)> env(SDL_CreateEnvironment(true), SDL_DestroyEnvironment);
	if (!env) throw std::runtime_error(SDL_GetError());
	for (auto pair : {std::pair{"OPENC2E_SESSION_PROFILE", copy.profile.string()},
		std::pair{"OPENC2E_SESSION_WORLD", active_world},
		std::pair{"OPENC2E_SESSION_DURATION_MS", std::to_string(duration)}}) {
		if (!SDL_SetEnvironmentVariable(env.get(), pair.first, pair.second.c_str(), true)) throw std::runtime_error(SDL_GetError());
	}
	auto props = SDL_CreateProperties();
	SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
	SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env.get());
	SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, copy.data.string().c_str());
	std::fflush(stdout);
	auto process = SDL_CreateProcessWithProperties(props);
	SDL_DestroyProperties(props);
	if (!process) throw std::runtime_error(SDL_GetError());
	std::unique_ptr<SDL_Process, decltype(&SDL_DestroyProcess)> child(process, SDL_DestroyProcess);
	int code = 0;
	while (!SDL_WaitProcess(child.get(), false, &code)) { SDL_PumpEvents(); SDL_Delay(20); }
	return code;
}
#undef main
int main(int argc, char** argv) {
	bool interactive = argc == 1;
	try {
		std::string data, world, requested = "auto", root;
		bool inspect_only = false, prepare_only = false, no_save = false, parse_only = false;
		std::vector<fs::path> sequence;
		int duration = 0;
		cxxopts::Options options("openc2e", "Select a C1 or C2 installation and world. Play uses an independent session copy.");
		options.add_options()
			("h,help", "Show help")
			("switch-check", "Test successive installations in one supervisor (comma separated; requires --run-for-ms)", cxxopts::value(sequence))
			("d,data-path", "Original installation (read only)", cxxopts::value(data))
			("w,world", "SFC world in that installation (default: saved world, World, Eden)", cxxopts::value(world))
			("g,game", "auto, c1 or c2 (validated)", cxxopts::value(requested))
			("session-root", "Directory for persistent session copies", cxxopts::value(root))
			("inspect", "Validate metadata without copying or launching", cxxopts::value(inspect_only))
			("prepare-only", "Prepare the session copy without launching", cxxopts::value(prepare_only))
			("no-save", "Disable C1 world saves (other changes remain in the copy)", cxxopts::value(no_save))
			("parse-only", "C2 parser/bootstrap check, without rendering or simulation", cxxopts::value(parse_only))
			("run-for-ms", "Stop the runtime after this many wall-clock milliseconds", cxxopts::value(duration));
		auto parsed = options.parse(argc, argv);
		if (parsed.count("help")) { fmt::print("{}\n", options.help()); return 0; }
		if (!sequence.empty() && (duration <= 0 || !data.empty() || !world.empty())) throw std::runtime_error("--switch-check requires --run-for-ms and uses each installation's default world");
		if (duration < 0) throw std::runtime_error("--run-for-ms must be nonnegative");
		if (interactive && !SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
		if (root.empty() && !inspect_only) {
			char* prefs = SDL_GetPrefPath("openc2e", "sessions");
			if (!prefs) throw std::runtime_error(SDL_GetError());
			root = prefs;
			SDL_free(prefs);
		}
		size_t sequence_index = 0;
		do {
			if (!sequence.empty()) data = sequence.at(sequence_index++).string();
			if (interactive) {
				const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose installation and world"}, {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"}};
				const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, "openc2e — Creatures 1 and 2",
					"Choose the folder containing Creatures.exe or Creatures2.exe, then its world. Play uses a separate copy. Close the world to return here. C2 compatibility is incomplete; saving C2 worlds is unavailable.", 2, buttons, nullptr};
				int pressed = 0;
				if (!SDL_ShowMessageBox(&box, &pressed) || pressed != 1) break;
				data = choose(true, nullptr);
				if (data.empty()) continue;
				world = choose(false, data.c_str());
				if (world.empty()) continue;
			}
			try {
				auto selection = session::inspect(data, world, requested);
				fmt::print("Selection: {} installation={} world={} (metadata validated; full parsing not yet checked)\n", session::name(selection.game), selection.installation.string(), selection.world.string());
				if (inspect_only) return 0;
				auto copy = session::prepare(selection, root);
				fmt::print("Session copy: {}\n", copy.root.string());
				if (prepare_only) return 0;
				int code = run(selection, copy, no_save, duration, parse_only);
				if (!interactive && (sequence.empty() || code)) return code;
				if (code) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "World stopped", "The world could not run successfully. See the session logs. C2 support is incomplete. The original installation is unchanged.", nullptr);
			} catch (const std::exception& e) {
				if (!interactive) throw;
				SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Unable to open world", e.what(), nullptr);
			}
		} while (interactive || sequence_index < sequence.size());
		SDL_Quit();
		return 0;
	} catch (const std::exception& e) { fmt::print(stderr, "openc2e: {}\n", e.what()); return 1; }
}
