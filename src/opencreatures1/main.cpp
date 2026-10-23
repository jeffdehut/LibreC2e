#include "common/SessionProfile.h"
#include "C1MusicManager.h"
#include "CobInjector.h"
#include "GameUI.h"
#include "UIStyle.h"
#include "Kits.h"
#include "SaveAndShare.h"
#include "UIImages.h"
#include "CreatureGenetics.h"
#include "Wolfling.h"
#include "Family.h"
#include "EggFiles.h"
#include "HdArt.h"
#include "C1SoundManager.h"
#include "EngineContext.h"
#include "ImageManager.h"
#include "Macro.h"
#include "MacroManager.h"
#include "MapManager.h"
#include "MessageManager.h"
#include "PathManager.h"
#include "PointerManager.h"
#include "SFCSerialization.h"
#include "ViewportManager.h"
#include "WorldState.h"
#include "WorldSystems.h"
#include "common/OptionsParser.h"
#include "common/StringView.h"
#include "CrashReport.h"
#include "common/io/FileWriter.h"
#include "common/render/RenderSystem.h"
#include "fileformats/sfc/SFCFile.h"
#include "objects/Blackboard.h"
#include "objects/CompoundObject.h"
#include "objects/Creature.h"
#include "objects/SimpleObject.h"
#include "objects/ObjectManager.h"
#include "sdlbackend/SDLBackend.h"
#include "sdlbackend/SDLMixerBackend.h"

#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
// on iOS, SDL starts the app and then calls main
#include <SDL3/SDL_main.h>
#endif
#include <cstdlib>
#include <imgui.h>
#include <imgui_internal.h>
#include "fileformats/sfc/MapData.h"
#include "fileformats/sfc/CGallery.h"
#include <stdint.h>

#ifdef _WIN32
#include "common/WindowsRegistry.h"
#endif

#include <chrono>
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <fmt/core.h>
#include <math.h>

namespace fs = std::filesystem;


// SDL tries stealing main on some platforms, which we don't want; except on iOS, where SDL
// has to start the app (SDL_main.h above).
#if !(defined(__APPLE__) && TARGET_OS_IPHONE)
#undef main
#endif

static RenderSystem s_rendersystem;

// What the game was doing, for crash reports.
static std::string describe_game_state() {
	std::string out;
	out += fmt::format("  World file: {}\n", get_world_file_name());
	out += fmt::format("  Game data:  {}\n", g_engine_context.paths->find_path(PATH_TYPE_MAIN, "").string());
	out += fmt::format("  Tick:       {}\n", g_engine_context.world->tick);
	out += fmt::format("  Paused:     {}, speed {}x\n", GameUI::paused(), GameUI::game_speed());
	size_t objects = 0;
	std::string creatures;
	for (auto* obj : *g_engine_context.objects) {
		objects++;
		if (auto* c = obj->as_creature()) {
			creatures += fmt::format("    {} genus {} sex {} age {}{}{} decision {}\n", c->moniker, c->genus, c->sex, c->age,
				c->dead ? " dead" : "", c->asleep ? " asleep" : "", c->current_decision);
		}
	}
	out += fmt::format("  Objects:    {}\n", objects);
	if (auto* selected = g_engine_context.objects->try_get(g_engine_context.world->current_norn)) {
		out += fmt::format("  Selected:   {}\n", format_as(selected));
	}
	out += "  Creatures:\n" + (creatures.empty() ? std::string("    none\n") : creatures);
	if (const Macro* m = g_running_macro) {
		const size_t from = m->ip > 120 ? m->ip - 120 : 0;
		out += fmt::format("  Running script: {} {} {} event {}, owner {}\n", m->classifier[0], m->classifier[1], m->classifier[2], m->event, m->ownr.to_integral());
		out += fmt::format("    ...{} <<HERE>> {}...\n", m->script.substr(from, m->ip - from), m->ip < m->script.size() ? m->script.substr(m->ip, 120) : "");
	} else {
		out += "  Running script: none\n";
	}
	return out;
}

static std::string s_datapath;

// World files the original uses are never written to: a world loaded from World.sfc or
// Eden.sfc is saved as World.openc1.sfc.
static std::string save_name_for(const std::string& file) {
	std::string lower;
	for (char c : file) {
		lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (lower == "world.sfc" || lower == "eden.sfc") {
		return "World.openc1.sfc";
	}
	return file;
}

// The shipped world's incubator (3 1 3) has always drawn its door (part 2) 2 px right and
// 3 px low: the door hole in its body picture is at (34, 59), but the door was placed at
// (36, 62), leaving a visible gap. Players used an add-on to fix it; here it's fixed when a
// world loads.
void fix_artwork_glitches() {
	for (auto* obj : *g_engine_context.objects) {
		// Not in the original: the hand can click the island's carrot vendor (2 8 3), which
		// only creatures could use; a click by the hand drops a carrot beside it (Object.cpp)
		if (obj->family == 2 && obj->genus == 8 && obj->species == 3) {
			if (auto* simple = obj->as_simple_object(); simple && simple->click_bhvr[0] < 0) {
				simple->click_bhvr = {MESSAGE_ACTIVATE1, -1, -1};
				obj->attr = static_cast<uint8_t>(obj->attr | ATTR_ACTIVATEABLE);
			}
		}
		if (auto* c = obj->as_creature(); c && c->genome) {
			restore_missing_genes(*c, *c->genome);
		}
		if (auto* c = obj->as_creature()) {
			seed_food_memory(*c);
		}
	}
	for (auto* obj : *g_engine_context.objects) {
		auto* comp = obj->as_compound_object();
		if (!comp || obj->family != 3 || obj->genus != 1 || obj->species != 3 || comp->parts.size() < 3) {
			continue;
		}
		auto& door = comp->parts[2];
		if (door.relx == 36 && door.rely == 62 && door.gallery().name == "incu") {
			door.relx = 34;
			door.rely = 59;
			auto main = obj->get_bbox();
			door.set_position(main.x + 34, main.y + 59);
			fmt::print("INFO [Fix] moved the incubator's door into its frame\n");
		}
	}
}

// Loads a world, replacing whatever is running. The camera options and the editing option
// carry over.
static void load_world(const fs::path& sfc_path, const std::string& file_name) {
	fmt::print("* Loading world: {:?}\n", sfc_path.string());
	auto sfc = sfc::read_sfc_v1_file(sfc_path);

	static bool had_world = false;
	const float scale = g_engine_context.viewport->scale;
	const bool smooth = g_engine_context.viewport->smooth_scrolling;
	const bool infinite = g_engine_context.viewport->infinite_scroll;
	const bool editing = g_engine_context.pointer->m_editing;
	const auto previous_name = get_world_file_name();
	std::unique_ptr<sfc::SFCFile> previous;
	if (had_world) {
		previous = std::make_unique<sfc::SFCFile>(sfc_dump_everything());
        Kits::flush_records();
        Kits::world_changed();
		g_engine_context.reset();
	}
	had_world = true;
	g_engine_context.paths->set_main_directory(s_datapath);
	g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
	g_engine_context.images->load_default_palette();
	g_engine_context.viewport->scale = scale;
	g_engine_context.viewport->smooth_scrolling = smooth;
	g_engine_context.viewport->infinite_scroll = infinite;
	g_engine_context.pointer->m_editing = editing;

	try {
		sfc_load_everything(sfc);
	} catch (...) {
		// Parsing succeeded but a referenced asset/genome may still fail to load.
		// Restore the live snapshot so Load never strands the player in a half-world.
		if (previous) {
			g_engine_context.reset();
			g_engine_context.paths->set_main_directory(s_datapath);
			g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
			g_engine_context.images->load_default_palette();
			sfc_load_everything(*previous);
			set_world_file_name(previous_name);
			g_engine_context.viewport->scale = scale;
			g_engine_context.viewport->smooth_scrolling = smooth;
			g_engine_context.viewport->infinite_scroll = infinite;
			g_engine_context.pointer->m_editing = editing;
			Kits::world_changed();
			Family::world_changed();
		}
		throw;
	}
	set_world_file_name(save_name_for(file_name));

	// fire init scripts
	for (auto* o : *g_engine_context.objects) {
		g_engine_context.macros->queue_script(o, o, SCRIPT_INITIALIZE);
	}
	Kits::world_changed();
	Family::world_changed();
	EggFiles::world_changed();
	fix_artwork_glitches();
	const bool new_world = GameUI::world_loaded();
	// Eden/World.sfc are templates saved under another name. They must not inherit
	// a run belonging to an older World.openc1.sfc at that destination.
	Wolfling::world_changed(new_world || file_name != get_world_file_name());
}

void load_everything() {
	// set up global objects
	set_backend(SDLBackend::get_instance());
	get_backend()->init("opencreatures1", OPENC2E_DEFAULT_WIDTH, OPENC2E_DEFAULT_HEIGHT);
	// debugging: OPENC2E_WINDOW_SIZE=<width>x<height> opens the window at that size, to check
	// the interface's layout at different resolutions
	if (const char* size = std::getenv("OPENC2E_WINDOW_SIZE")) {
		int w = 0, h = 0;
		if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w > 200 && h > 200) {
			if (auto* sdl = dynamic_cast<SDLBackend*>(get_backend())) {
				SDL_SetWindowSize(sdl->window, w, h);
			}
		}
	}

	set_audio_backend(SDLMixerBackend::get_instance());
	get_audio_backend()->init(); // TODO: initialized early so SFC sounds can start.. is this right?

	g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);

	// load palette
	g_engine_context.images->load_default_palette();

	// the egg archive files eggs by their parents' generations, from the family registry
	EggFiles::set_lookups([](const std::string& m) { return Family::generation_of(m); }, [](const std::string& m) {
		const auto* member = Family::find(m);
		return member && member->name != "<Un Named>" ? member->name : std::string();
	});
	// Higher-resolution art (the modern layout's): LibreCreatures' NAME@2x.s32 files, from an
	// "Updated Sprites" folder in or beside the game folder, or beside the sprites in Images
	{
		const fs::path game = s_datapath;
		std::vector<fs::path> folders;
		if (const char* art = std::getenv("OPENC1_HD_ART")) {
			folders.emplace_back(art);
		}
		folders.push_back(game / "Updated Sprites");
		folders.push_back(game.parent_path() / "Updated Sprites");
		folders.push_back(g_engine_context.paths->find_path(PATH_TYPE_IMAGE, ""));
		HdArt::set_folders(folders);
		HdArt::set_loader([](const std::vector<uint8_t>& png, int32_t width, int32_t height) -> Texture {
			auto* sdl = dynamic_cast<SDLBackend*>(get_backend());
			SDL_Renderer* renderer = sdl ? SDL_GetRenderer(sdl->window) : nullptr;
			if (!renderer) {
				return {};
			}
			SDL_Surface* surface = SDL_LoadPNG_IO(SDL_IOFromConstMem(png.data(), png.size()), true);
			if (!surface) {
				return {};
			}
			SDL_Texture* texture = surface->w == width && surface->h == height ? SDL_CreateTextureFromSurface(renderer, surface) : nullptr;
			SDL_DestroySurface(surface);
			if (!texture) {
				return {};
			}
			SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
			// drawn at half size or zoomed: smooth, unlike the 1x sprites' hard pixels
			SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
			return Texture(texture, width, height, &SDL_DestroyTexture);
		});
	}
	// each world's files in its own folder (moved there from the game folder, once)
	try {
		organise_world_files();
	} catch (const std::exception& e) {
		fmt::print(stderr, "* Couldn't organise the world files: {}\n", e.what());
	}
	// The shared entry point has already validated and copied the chosen world.
	if (const char* selected = std::getenv("OPENC2E_SESSION_WORLD")) {
		auto path = find_world(selected);
		if (path.empty()) throw Exception("Selected session world is missing");
		load_world(path, path.filename().string());
		return;
	}
	// Continue the last successfully saved world for this installation.
	if (const auto name = last_saved_world(); !name.empty()) {
		try { load_world(find_world(name), name); return; }
		catch (const std::exception& e) { fmt::print(stderr, "* Continue failed: {}\n", e.what()); }
	}
	// load World.openc1.sfc/World.sfc/Eden.sfc
	for (const char* name : {"World.openc1.sfc", "World.sfc", "Eden.sfc"}) {
		auto sfc_path = find_world(name);
		if (!sfc_path.empty()) {
			load_world(sfc_path, sfc_path.filename().string());
			return;
		}
		fmt::print(stderr, "* Couldn't find {}\n", name);
	}
	fmt::print(stderr, "* Error: Couldn't find a world to load\n");
	exit(1);
}

// --- the soak test ---
// OPENCREATURES1_SOAK_HOURS=<n> runs n game hours (36000 ticks each) as fast as the computer
// allows, reporting memory and counts every game hour, then quits. For checking that the game
// can be left running for weeks (a week at 1x is about 6.7 million ticks).

int soak_hours() {
	static const int hours = std::getenv("OPENCREATURES1_SOAK_HOURS") ? std::atoi(std::getenv("OPENCREATURES1_SOAK_HOURS")) : 0;
	return hours;
}

double resident_memory_mb() {
#if defined(__APPLE__)
	mach_task_basic_info info{};
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
		return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
	}
	return 0;
#elif defined(_WIN32)
	PROCESS_MEMORY_COUNTERS counters{};
	if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
		return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
	}
	return 0;
#else
	long pages = 0, resident = 0;
	if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
		if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) {
			resident = 0;
		}
		std::fclose(f);
	}
	return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
}

std::chrono::steady_clock::time_point s_soak_started = std::chrono::steady_clock::now();

void soak_report(uint64_t ticks_run) {
	const auto started = s_soak_started;
	int objects = 0, creatures = 0, alive = 0;
	for (auto* obj : *g_engine_context.objects) {
		objects++;
		if (auto* c = obj->as_creature()) {
			creatures++;
			alive += c->dead ? 0 : 1;
		}
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::error_code err;
	const auto log_size = std::filesystem::file_size(CrashReport::session_log_path(), err);
	fmt::print("SOAK hour {}: tick {}, {:.0f} ticks/s, memory {:.1f} MB, objects {}, creatures {} ({} alive), scripts {}, log {} KB\n",
		ticks_run / 36000, g_engine_context.world->tick, static_cast<double>(ticks_run) / std::max(seconds, 0.001), resident_memory_mb(),
		objects, creatures, alive, g_engine_context.macros->count(), err ? 0 : log_size / 1024);
}

void update_everything() {
	// these should update as often as possible, regardless of ticks
	g_engine_context.music->update();
	g_engine_context.sounds->update();

	// some things can update only every "tick" - usually described as
	// 1/10sec but actually every 90 milliseconds, or 1/11.1̅ sec
	using namespace std::chrono;
	static steady_clock::duration accumulator{0};
	static time_point<steady_clock> prev_frame_time{steady_clock::now()};
	auto now = steady_clock::now();
	accumulator += (now - prev_frame_time);
	// the camera moves every frame, in real time, paused or not
	g_engine_context.viewport->frame(duration<float>(now - prev_frame_time).count());
	prev_frame_time = now;

	if (GameUI::paused()) {
		accumulator = steady_clock::duration{0};
		// F033: the world doesn't advance (this also blocks Max-speed and soak scheduling).
		// But the player can still work while paused: the hand follows the mouse, and scripts
		// the hand started (injected COBs, kit ADD items, which put the new object in the hand)
		// run, so objects can be placed before resuming.
		g_engine_context.macros->tick_owned_by(g_engine_context.pointer->m_pointer_tool);
		g_engine_context.objects->destroy_pending();
		g_engine_context.pointer->update();
		return;
	}
	// A tick is 90 ms, shorter when the player speeds the game up. Each frame runs the ticks
	// that are due, but spends at most TICK_BUDGET on them: when the game can't keep up (a
	// busy world at 4x, a slow machine), it runs as fast as it can instead of falling ever
	// further behind and freezing the window. Time it couldn't catch up on is dropped.
	const auto TICK_BUDGET = milliseconds(soak_hours() > 0 ? 250 : 40);
	auto tick_length = duration_cast<steady_clock::duration>(milliseconds(90 / std::max(1, GameUI::game_speed())));
	if (accumulator > seconds(1)) {
		accumulator = seconds(1); // after a long stall (e.g. the window was dragged)
	}
	if (soak_hours() > 0) {
		// as many ticks as fit in the budget
		accumulator = seconds(1);
		tick_length = steady_clock::duration{1};
	}
	const auto frame_start = steady_clock::now();
	while (accumulator >= tick_length) {
		if (steady_clock::now() - frame_start > TICK_BUDGET) {
			accumulator = steady_clock::duration{0};
			break;
		}

		accumulator -= tick_length;

		// the system tick, saved in the world file; queued messages and script stimuli are
		// delivered every 4 ticks (spec Q13)
		g_engine_context.world->tick++;
		GameUI::tick();
		g_engine_context.objects->tick();
		disease_world_tick();
		world_systems_tick();
		EggFiles::tick();
		g_engine_context.macros->tick();
		if (g_engine_context.world->tick % 4 == 0) {
			g_engine_context.messages->tick();
		}
		g_engine_context.objects->destroy_pending();

		if (soak_hours() > 0) {
			static uint64_t soak_ticks = 0;
			if (++soak_ticks % 36000 == 0) {
				soak_report(soak_ticks);
				if (soak_ticks >= static_cast<uint64_t>(soak_hours()) * 36000) {
					fmt::print("SOAK finished\n");
					std::fflush(stdout);
					SDL_Delay(500); // let the log catch up
					std::_Exit(0);
				}
			}
		}

		static auto prev_second = duration_cast<seconds>(steady_clock::now().time_since_epoch());
		static uint64_t ticks = 0;
		auto current_second = duration_cast<seconds>(steady_clock::now().time_since_epoch());
		if (current_second != prev_second) {
			prev_second = current_second;
			if (soak_hours() == 0) {
				printf("ticks this second %lli\n", ticks);
			}
			ticks = 0;
		}
		ticks++;
	}

	// these should update as often as possible, regardless of ticks
	// update pointer after viewport
	g_engine_context.pointer->update();
}

void draw_everything() {
	HdArt::update(); // decode some of the higher-resolution frames asked for
	s_rendersystem.clear();
	g_engine_context.map->render(s_rendersystem);
	for (const auto* object : *g_engine_context.objects) {
		object->render(s_rendersystem);
	}

	// views the interface shows (Creature's View) go first, so the main view is drawn last
	GameUI::render_world_views(s_rendersystem);

	RenderSystem::DrawConfig drawconfig;
	drawconfig.world_src = g_engine_context.viewport->get_main_camera_rect();
	drawconfig.world_wrap_width = g_engine_context.map->get_world_wrap_width();
	drawconfig.screen_dest = g_engine_context.viewport->get_screen_dest_rect();
	drawconfig.renderer = get_backend()->getMainRenderTarget();
	s_rendersystem.draw(drawconfig);
}

int32_t get_fps() {
	static std::chrono::steady_clock::time_point time_of_last_render{};
	static std::array<std::chrono::milliseconds::rep, 60> rendertimes{};
	static size_t rendertimeptr = 0;

	auto n = std::chrono::steady_clock::now();
	rendertimes[rendertimeptr++] =
		std::chrono::duration_cast<std::chrono::milliseconds>(n - time_of_last_render).count();
	if (rendertimeptr == rendertimes.size()) {
		rendertimeptr = 0;
	}
	time_of_last_render = n;
	decltype(rendertimes)::value_type avgtime = 0;
	for (auto t : rendertimes) {
		avgtime += t;
	}
	return 60000 / avgtime;
}

// Whether a folder holds Creatures 1 (its Creatures.exe, in any letter case).
// A Creatures 1 installation: the program and the files the game can't run without (its
// world, pictures and genomes). The player's own copy is the only source of the game's
// assets; nothing of the game is shipped.
static bool is_game_folder(const fs::path& folder) {
	std::error_code err;
	if (folder.empty() || !fs::is_directory(folder, err)) {
		return false;
	}
	bool exe = false, world = false, images = false, genetics = false;
	for (auto& entry : fs::directory_iterator(folder, err)) {
		std::string name = entry.path().filename().string();
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		exe = exe || name == "creatures.exe";
		world = world || name == "eden.sfc";
		images = images || (name == "images" && entry.is_directory(err));
		genetics = genetics || (name == "genetics" && entry.is_directory(err));
	}
	return exe && world && images && genetics;
}

// Where players' copies usually are: the original's own registry entry, then the folders GOG,
// GOG Galaxy, its Mac app, CrossOver, Wine and Lutris use. Each candidate named like the game
// is checked, and the folders a level or two inside it (a Mac app keeps the game deeper).
static std::string find_installed_game() {
	std::error_code err;
	std::vector<fs::path> direct;
#ifdef _WIN32
	for (auto root : {REGISTRY_HKEY_CURRENT_USER, REGISTRY_HKEY_LOCAL_MACHINE}) {
		for (const char* key : {"Software\\Gameware Development\\Creatures 1\\1.0", "Software\\WOW6432Node\\Gameware Development\\Creatures 1\\1.0"}) {
			try {
				const std::string folder = registry_get_string_value(root, key, "Main Directory");
				if (!folder.empty()) {
					direct.emplace_back(folder);
				}
			} catch (...) {
			}
		}
	}
#endif
	for (const auto& folder : direct) {
		if (is_game_folder(folder)) {
			return folder.string();
		}
	}
	std::vector<fs::path> bases;
	const char* home = std::getenv("HOME");
#ifdef _WIN32
	if (!home) {
		home = std::getenv("USERPROFILE");
	}
	for (const char* drive : {"C:", "D:"}) {
		for (const char* place : {"\\GOG Games", "\\Program Files (x86)\\GOG Galaxy\\Games", "\\Program Files\\GOG Galaxy\\Games", "\\Program Files (x86)\\GOG.com", "\\Program Files (x86)", "\\Program Files", "\\Games"}) {
			bases.emplace_back(std::string(drive) + place);
		}
	}
#endif
	if (home) {
		const fs::path h(home);
		for (const char* place : {"GOG Games", "Games", "Applications", "Documents", "Desktop", ".wine/drive_c/GOG Games", ".wine/drive_c/Program Files (x86)/GOG Galaxy/Games",
				 ".wine/drive_c/Program Files (x86)", ".wine/drive_c/Program Files"}) {
			bases.push_back(h / place);
		}
		// CrossOver bottles on a Mac, and Lutris prefixes on Linux
		for (const fs::path& bottles : {h / "Library/Application Support/CrossOver/Bottles", h / "Games"}) {
			for (auto& bottle : fs::directory_iterator(bottles, err)) {
				for (const char* place : {"drive_c/GOG Games", "drive_c/Program Files (x86)/GOG Galaxy/Games", "drive_c/Program Files (x86)", "drive_c/Program Files"}) {
					bases.push_back(bottle.path() / place);
				}
			}
		}
	}
	bases.emplace_back("/Applications");
	for (const auto& base : bases) {
		for (auto& entry : fs::directory_iterator(base, err)) {
			std::string name = entry.path().filename().string();
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (name.rfind("creatures", 0) != 0 || !entry.is_directory(err) || name.find("2") != std::string::npos || name.find("3") != std::string::npos) {
				continue;
			}
			if (is_game_folder(entry.path())) {
				return entry.path().string();
			}
			// a level or two inside: Creatures.app/Contents/Resources/game, or a folder of the game
			fs::recursive_directory_iterator inner(entry.path(), fs::directory_options::skip_permission_denied, err);
			for (; !err && inner != fs::recursive_directory_iterator(); inner.increment(err)) {
				if (inner.depth() >= 3) {
					inner.disable_recursion_pending();
				}
				if (inner->is_directory(err) && is_game_folder(inner->path())) {
					return inner->path().string();
				}
			}
			err.clear();
		}
	}
	return "";
}

static fs::path remembered_folder_file() {
	// a test profile keeps its own, so tests never change the player's
	if (auto profile = session_profile(); !profile.empty()) {
		return profile / "game folder.txt";
	}
	char* dir = SDL_GetPrefPath("openc2e", "opencreatures1");
	fs::path path = dir ? fs::path(dir) / "game folder.txt" : fs::path();
	SDL_free(dir);
	return path;
}

// Without a folder on the command line: the folder chosen last time, the one the program is
// in, the current one, or else the player picks it (and it's remembered).
static std::string find_game_folder(const char* argv0) {
	std::error_code err;
#if defined(__APPLE__) && TARGET_OS_IPHONE
	// iPad and iPhone: the player copies their own Creatures 1 folder (from the GOG game) into
	// the app's Documents, through Finder or the Files app. The game's files can't be shipped.
	(void)argv0;
	const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
	if (documents) {
		const fs::path root(documents);
		if (is_game_folder(root)) {
			return root.string();
		}
		for (const auto& entry : fs::directory_iterator(root, err)) {
			if (entry.is_directory(err) && is_game_folder(entry.path())) {
				return entry.path().string();
			}
		}
	}
	SDL_Init(SDL_INIT_VIDEO);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Creatures",
		"Copy your Creatures 1 folder (the one with Creatures.exe in it, from the GOG game) into "
		"this app's folder, then open the app again.\n\n"
		"From a Mac: connect this device, select it in Finder, choose Files, and drag the folder "
		"onto Creatures.\n\n"
		"On this device: in the Files app, put the folder in On My iPad (or iPhone) > Creatures.",
		nullptr);
	return "";
#endif
	{
		std::ifstream in(remembered_folder_file());
		std::string saved;
		std::getline(in, saved);
		if (is_game_folder(saved)) {
			return saved;
		}
	}
	for (fs::path candidate : {fs::absolute(fs::path(argv0), err).parent_path(), fs::current_path(err)}) {
		if (is_game_folder(candidate)) {
			return candidate.string();
		}
	}
	if (auto found = find_installed_game(); !found.empty()) {
		fmt::print("* Found Creatures 1 at {:?}\n", found);
		std::ofstream(remembered_folder_file()) << found;
		return found;
	}
	SDL_Init(SDL_INIT_VIDEO);
	std::string chosen;
	bool done = false;
	const SDL_MessageBoxButtonData buttons[] = {
		{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose folder..."},
		{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
	};
	while (!done) {
		const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, "Creatures",
			"Where is Creatures 1 installed? Choose the folder that has Creatures.exe in it (for example the GOG game's folder). This program uses your own copy of the game: it doesn't come with the game's files.", 2, buttons, nullptr};
		int pressed = 0;
		if (!SDL_ShowMessageBox(&box, &pressed) || pressed != 1) {
			return "";
		}
		bool answered = false;
		std::string picked;
		struct Result {
			bool* answered;
			std::string* picked;
		} result{&answered, &picked};
		SDL_ShowOpenFolderDialog([](void* userdata, const char* const* files, int) {
			auto* r = static_cast<Result*>(userdata);
			if (files && files[0]) {
				*r->picked = files[0];
			}
			*r->answered = true; }, &result, nullptr, nullptr, false);
		while (!answered) {
			SDL_PumpEvents();
			SDL_Delay(20);
		}
		if (picked.empty()) {
			return "";
		}
		if (is_game_folder(picked)) {
			chosen = picked;
			done = true;
		} else {
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Creatures", "That folder doesn't look like a Creatures 1 installation (it needs Creatures.exe, Eden.sfc and the Images and Genetics folders). Please choose the Creatures 1 folder.", nullptr);
		}
	}
	std::ofstream(remembered_folder_file()) << chosen;
	return chosen;
}

int main(int argc, char** argv) {
	CrashReport::install();
	CrashReport::set_state_describer(describe_game_state);

	try {
	bool no_save = false;
	bool inject_all = false;
	std::string datapath;
	auto opts = OptionsParser{}
					.opt("--no-save", &no_save)
					.opt("--inject-all", &inject_all) // debugging: inject every COB and RCB at startup
					.opt("datapath", &datapath)
					.parse(argc, argv);
#ifdef _WIN32
	if (datapath.empty()) {
		datapath = registry_get_string_value(REGISTRY_HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Gameware Development\\Creatures 1\\1.0", "Main Directory");
		if (datapath.empty()) {
			fmt::print(stderr, "Couldn't find Creatures 1 registry key\n");
		}
	}
#endif
	if (datapath.empty()) {
		datapath = find_game_folder(argc > 0 ? argv[0] : "");
	}
	if (datapath.empty()) {
		return 0; // the player cancelled
	}

	if (!fs::exists(datapath)) {
		fmt::print(stderr, "* Error: Data path {:?} does not exist\n", datapath);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Creatures", fmt::format("The Creatures folder \"{}\" doesn't exist.", datapath).c_str(), nullptr);
		return 1;
	}

	if (no_save) {
		set_autosave_enabled(false);
	}
	fmt::print("* Creatures 1 Data: {:?}\n", datapath);
	s_datapath = datapath;
	g_engine_context.paths->set_main_directory(datapath);
	GameUI::load_options();
	load_everything();
	// debugging: OPENCREATURES1_TEST_VIEW=x,y,scale starts the camera there (for screenshots)
	if (const char* view = std::getenv("OPENCREATURES1_TEST_VIEW")) {
		float x = 0, y = 0, scale = 1;
		if (std::sscanf(view, "%f,%f,%f", &x, &y, &scale) == 3) {
			g_engine_context.viewport->scale = scale;
			g_engine_context.viewport->centre_on(x, y);
		}
	}
	UIStyle::init();
	UIStyle::set_scale(GameUI::options().ui_scale);
	GameUI::show_startup_tip();
	// debugging: OPENCREATURES1_KITS opens kits at startup, e.g. "Owner,Health" or "all"
	if (const char* kits = std::getenv("OPENCREATURES1_KITS")) {
		Kits::open_for_testing(kits);
		// with a subject for them: the first live creature, if none is chosen
		if (!g_engine_context.objects->try_get(g_engine_context.world->current_norn)) {
			for (auto* obj : *g_engine_context.objects) {
				if (auto* c = obj->as_creature(); c && !c->dead) {
					g_engine_context.world->current_norn = c->uid;
					break;
				}
			}
		}
	}
	// debugging: OPENCREATURES1_WOLFLING=restock|line|none starts a relaxed wolfling run
	if (const char* wolfling = std::getenv("OPENCREATURES1_WOLFLING")) {
		Wolfling::Settings settings;
		settings.food = std::string(wolfling) == "line" ? Wolfling::FOOD_LINE : std::string(wolfling) == "none" ? Wolfling::FOOD_NONE : Wolfling::FOOD_RESTOCK;
		settings.snapshot_hours = 12;
		// OPENCREATURES1_WOLFLING_FLOOR=<n>: keep adding eggs until n Norns are alive (to test
		// crowded worlds)
		if (const char* floor = std::getenv("OPENCREATURES1_WOLFLING_FLOOR")) {
			settings.population_floor = std::atoi(floor);
		}
		Wolfling::start(settings);
	}
	// debugging: OPENCREATURES1_SPEED starts the game sped up (1, 2 or 4)
	if (const char* speed = std::getenv("OPENCREATURES1_SPEED")) {
		GameUI::set_game_speed(std::atoi(speed));
	}
	// debugging: OPENCREATURES1_TYPE puts text in the speech box
	if (const char* typed = std::getenv("OPENCREATURES1_TYPE")) {
		GameUI::set_typed_text_for_testing(typed);
	}
	// debugging: OPENCREATURES1_SCRIPT runs a script at startup, owned by the pointer
	if (const char* script = std::getenv("OPENCREATURES1_SCRIPT")) {
		Macro m(script);
		m.ownr = m.targ = g_engine_context.pointer->m_pointer_tool;
		g_engine_context.macros->add(m);
	}
	if (inject_all) {
		for (const auto& cob : list_injectable_cobs()) {
			try {
				inject_cob(cob);
			} catch (Exception& e) {
				fmt::print(stderr, "* Couldn't inject {:?}: {}\n", cob.string(), e.what());
			}
		}
	}

	// Integration regression hook: writes only inside an isolated session copy.
	if (std::getenv("OPENC2E_SESSION_TEST_PAUSE")) {
		if (session_profile().empty()) throw Exception("Pause regression requires an isolated session");
		const auto tick = g_engine_context.world->tick;
		const auto original_name = get_world_file_name();
		GameUI::set_paused(true);
		for (int speed : {1, 2, 4, 99}) {
			GameUI::set_game_speed(speed);
			save_world(get_world_file_name());
			set_world_file_name("Pause Check.sfc");
			save_world(get_world_file_name());
			SDL_Delay(120);
			update_everything();
			if (!GameUI::paused() || g_engine_context.world->tick != tick) throw Exception("F033: paused world advanced across save/speed change");
		}
		set_world_file_name(original_name);
		GameUI::set_game_speed(1);
		GameUI::set_paused(false);
		SDL_Delay(120);
		update_everything();
		if (g_engine_context.world->tick == tick) throw Exception("F033: world did not resume");
		fmt::print("TEST F033 PASS: save, Save As, speeds 1/2/4/Max preserve tick {}; resume advances to {}\n", tick, g_engine_context.world->tick);
	}
    // Native UI integration check, restricted to an explicitly marked scratch copy.
    const bool kit_check = std::getenv("OPENC1_KIT_CHECK") != nullptr;
    if (kit_check) {
        if (session_profile().empty() || !fs::exists(fs::path(datapath) / ".kit-test-scratch"))
            throw Exception("Kit checks require an isolated profile and .kit-test-scratch data copy");
        if (const char* specimen = std::getenv("OPENC1_KIT_CHECK_IMPORT")) {
            for (int n = 0; n < 20; ++n) {
                auto* c = import_creature(specimen, 2591, 927, false);
                if (n == 0) g_engine_context.world->current_norn = c->uid;
            }
        }
        save_world("Kit Check.sfc");
        GameUI::set_paused(true);
        Kits::open_for_testing("all");
        // Pixel regression for F066: the entire bottom row must survive capture.
        RenderSystem pattern;
        const auto& palette = g_engine_context.images->default_palette();
        pattern.add(create_render_item_rect(Rect2f{0, 0, 16, 7}, palette[10]));
        pattern.add(create_render_item_rect(Rect2f{0, 8, 16, 7}, palette[200]));
        UIImages::request_photo(Rect2i{0, 0, 16, 16}, [](int32_t w, int32_t h, const std::vector<uint8_t>& pixels) {
            fmt::print("TEST F066 pixels: {}x{}, {} bytes, first {}, last {}\n", w, h, pixels.size(), pixels.empty() ? -1 : pixels.front(), pixels.empty() ? -1 : pixels.back());
            const auto& pal = g_engine_context.images->default_palette();
            const auto same_rgb = [&](uint8_t a, uint8_t b) { return pal[a].r == pal[b].r && pal[a].g == pal[b].g && pal[a].b == pal[b].b; };
            if (w != 16 || h != 16 || pixels.size() != 256 || !same_rgb(pixels.front(), 10) || !same_rgb(pixels.back(), 200))
                throw Exception("F066 photograph rectangle/orientation regression");
            fmt::print("TEST F066 PASS: 16x16 capture preserves first and last rows\n");
        });
        UIImages::take_requested_photo(pattern);
        fmt::print("TEST KIT POPULATION: {} creatures before save/reload\n", count_living_creatures());
    }
    const bool polish_check = std::getenv("OPENC1_POLISH_CHECK") != nullptr;
    if (polish_check) {
        if (session_profile().empty() || !fs::exists(fs::path(datapath) / ".kit-test-scratch"))
            throw Exception("Polish checks require an isolated marked scratch copy");
        const auto tick = g_engine_context.world->tick;
        const auto living = count_living_creatures();
        const auto before_name = get_world_file_name();
        save_world("Polish Check.sfc");
        auto broken = sfc_dump_everything();
        broken.map->background->filename = "xxxx";
        {
            FileWriter writer(fs::path(datapath) / "Polish Invalid.sfc");
            sfc::write_sfc_v1_file(writer, broken);
            writer.close();
        }
        bool rejected = false;
        try { load_world(fs::path(datapath) / "Polish Invalid.sfc", "Polish Invalid.sfc"); }
        catch (const std::exception&) { rejected = true; }
        if (!rejected || g_engine_context.world->tick != tick || count_living_creatures() != living || get_world_file_name() != before_name)
            throw Exception("Failed-load rollback did not preserve the current world");
        fs::remove(fs::path(datapath) / "Polish Invalid.sfc");
        for (int attempt = 0; attempt < 3; ++attempt) {
            GameUI::set_paused(true);
            load_world(find_world("Polish Check.sfc"), "Polish Check.sfc");
            if (!GameUI::paused() || count_living_creatures() != living)
                throw Exception("Repeated world load lost pause or population");
        }
        GameUI::set_paused(false);
        fmt::print("TEST POLISH LOAD PASS: failed asset load restored tick/population/name; three reloads preserved pause\n");
    }
    if (std::getenv("OPENC1_WATCHDOG_CHECK")) {
        if (session_profile().empty() || !fs::exists(fs::path(datapath) / ".kit-test-scratch"))
            throw Exception("Watchdog checks require an isolated marked scratch copy");
        CrashReport::heartbeat();
        CrashReport::begin_native_wait();
        SDL_Delay(9500);
        CrashReport::end_native_wait();
        if (fs::is_directory(CrashReport::reports_folder())) {
            for (const auto& file : fs::directory_iterator(CrashReport::reports_folder()))
                if (file.path().filename().string().rfind("not responding", 0) == 0)
                    throw Exception("Native wait incorrectly generated a hang report");
        }
        fmt::print("TEST WATCHDOG PASS: expected 9.5-second native wait did not generate a hang report\n");
    }
    int polish_frame = 0;
    int kit_check_frame = 0;
	// run loop
	const auto initial_tick = g_engine_context.world->tick;
	const auto session_start = std::chrono::steady_clock::now();
	const char* duration_env = std::getenv("OPENC2E_SESSION_DURATION_MS");
	const auto session_duration = duration_env ? std::strtoll(duration_env, nullptr, 10) : 0;
	get_backend()->run([&] {
        if (polish_check) {
            ++polish_frame;
            auto& io = ImGui::GetIO();
            if (polish_frame == 5) io.AddFocusEvent(true);
            if (polish_frame == 10) io.AddKeyEvent(ImGuiKey_F9, true);
            if (polish_frame == 11) io.AddKeyEvent(ImGuiKey_F9, false);
            if (polish_frame == 20) {
                auto* window = ImGui::FindWindowByName("Load world");
                if (!window || !window->WasActive || !GameUI::paused()) throw Exception("Load browser shortcut/pause failed");
                fmt::print("TEST POLISH UI: load browser visible and simulation paused\n");
            }
            if (polish_frame == 30 || polish_frame == 60) io.AddKeyEvent(ImGuiKey_Escape, true);
            if (polish_frame == 31 || polish_frame == 61) io.AddKeyEvent(ImGuiKey_Escape, false);
            if (polish_frame == 40) { io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiMod_Shift, true); io.AddKeyEvent(ImGuiKey_S, true); }
            if (polish_frame == 41) { io.AddKeyEvent(ImGuiKey_S, false); io.AddKeyEvent(ImGuiMod_Shift, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); }
            if (polish_frame == 50) {
                auto* window = ImGui::FindWindowByName("Save game as");
                if (!window || !window->WasActive || !GameUI::paused()) throw Exception("Save dialog shortcut/pause failed");
                fmt::print("TEST POLISH UI: named save dialog visible and simulation paused\n");
            }
            if (polish_frame == 80) {
                if (GameUI::paused()) throw Exception("Closing world dialogs did not restore resume state");
                fmt::print("TEST POLISH UI PASS: load/save keyboard flow, cancel and resume\n");
                return false;
            }
        }
        if (kit_check) {
            ++kit_check_frame;
            if (kit_check_frame == 64 || kit_check_frame == 160) {
                Kits::world_changed();
                int windows = 0; auto** all = SDL_GetWindows(&windows); SDL_free(all);
                if (windows != 1) throw Exception("Kit closure left stale native windows");
                fmt::print("TEST KIT LIFECYCLE: closed all windows and discarded transient state\n");
            }
            if (kit_check_frame == 65 || kit_check_frame == 161) Kits::open_for_testing("all");
            if (kit_check_frame == 66 || kit_check_frame == 98 || kit_check_frame == 162) {
                int windows = 0; auto** all = SDL_GetWindows(&windows); SDL_free(all);
                const int expected = UIStyle::classic() ? 11 : 1;
                if (windows != expected) throw Exception("Native kit slot/window count did not recover");
                fmt::print("TEST KIT WINDOWS: {} windows, expected {}\n", windows, expected);
            }
            if (kit_check_frame == 96) {
                load_world(find_world("Kit Check.sfc"), "Kit Check.sfc");
                GameUI::set_paused(true);
                Kits::open_for_testing("all");
                fmt::print("TEST KIT RELOAD: {} creatures after reload\n", count_living_creatures());
            }
            if (kit_check_frame == 192) {
                g_engine_context.world->current_norn = {};
                fmt::print("TEST KIT SUBJECT: no selected creature\n");
            }
            if (kit_check_frame == 224) {
                for (auto* obj : *g_engine_context.objects) if (obj->as_creature()) {
                    g_engine_context.world->current_norn = obj->uid; break;
                }
                GameUI::set_paused(false);
            }
            if (kit_check_frame == 256) {
                fmt::print("TEST KIT PASS: pages, close/reopen, save/reload, no-subject and resume completed\n");
                return false;
            }
        }
		// handle ui events
		BackendEvent event;
		while (get_backend()->pollEvent(event)) {
			g_engine_context.viewport->handle_event(event);
			g_engine_context.pointer->handle_event(event);
		}

		// a world the player asked for, loaded before anything else this frame
		if (auto file = GameUI::take_world_to_load(); !file.empty()) {
			auto path = find_world(file);
			try {
				if (path.empty()) {
					throw Exception(fmt::format("couldn't find {}", file));
				}
				load_world(path, file);
			} catch (std::exception& e) {
				fmt::print(stderr, "* Couldn't load {:?}: {}\n", file, e.what());
				GameUI::world_load_failed();
				GameUI::show_message(fmt::format("Couldn't open {}: {}", file, e.what()));
			}
		}

		// all game sounds stop while the window is in the background or minimised, as in the
		// original (batch 4, Q23)
		g_engine_context.sounds->set_muted(SDL_GetKeyboardFocus() == nullptr && !GameUI::options().sound_in_background);

		// debugging: OPENCREATURES1_TEST_CRASH=memory|exception|hang crashes (or hangs for 12 s)
		// on purpose after 3 s, to test crash and not-responding reports
		if (const char* crash = std::getenv("OPENCREATURES1_TEST_CRASH")) {
			static auto start = std::chrono::steady_clock::now();
			if (std::chrono::steady_clock::now() - start > std::chrono::seconds(3)) {
				if (std::string(crash) == "exception") {
					throw Exception("a test exception from OPENCREATURES1_TEST_CRASH");
				}
				if (std::string(crash) == "hang") {
					static auto until = std::chrono::steady_clock::now() + std::chrono::seconds(12);
					while (std::chrono::steady_clock::now() < until) {
						// stuck, as a hang would be (then carries on)
					}
				} else {
					volatile int* nowhere = nullptr;
					*nowhere = 1;
				}
			}
		}
		CrashReport::heartbeat();

		// the interface
		GameUI::draw();

		// update world
		update_everything();

		// draw
		draw_everything();
		if (session_duration > 0 && std::chrono::steady_clock::now() - session_start >= std::chrono::milliseconds(session_duration)) return false;
		return !GameUI::quit_requested();
	});

	fmt::print("SESSION C1: parsed=yes rendered=yes simulation_ticks={} final_tick={}\n", g_engine_context.world->tick - initial_tick, g_engine_context.world->tick);

    Kits::flush_records();
	// save world data
	if (no_save) {
		fmt::print("* Not saving\n");
	} else {
		save_world(get_world_file_name());
		fmt::print("* Saved world to: {}\n", get_world_file_name());
	}

	if (!no_save && !session_profile().empty()) {
		const auto name = get_world_file_name();
		if (fs::path(name).filename() != fs::path(name)) throw Exception("Session world must stay inside its copied installation");
		std::ofstream record(session_profile() / "active-world.txt");
		record.exceptions(std::ios::failbit | std::ios::badbit);
		record << name << '\n';
		record.close();
	}
	GameUI::save_options();

	// explicitly destroy game data
	// C1ControlledSounds need to be destroyed before the AudioBackend is destroyed,
	// because they'll try to stop their AudioChannel.
    Kits::world_changed();
	g_engine_context.reset();
	if (auto* audio = get_audio_backend()) audio->shutdown();

	return 0;
	} catch (const std::exception& e) {
        fmt::print(stderr, "World startup/runtime failed: {}\n", e.what());
        // Error exits need the same resource ordering as a normal shutdown. Static
        // SDL_mixer tracks must not outlive their mixer after a rejected world.
        Kits::world_changed();
        g_engine_context.reset();
        if (auto* audio = get_audio_backend()) audio->shutdown();
        return 1;
	}

}
