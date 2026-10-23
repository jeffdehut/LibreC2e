#include "WorldSystems.h"
#include "Wolfling.h"

#include "EngineContext.h"
#include "PathManager.h"
#include "SFCSerialization.h"
#include "WorldState.h"
#include "PersistentFile.h"
#include "common/Ascii.h"
#include "common/SessionProfile.h"
#include "common/io/VectorWriter.h"
#include "common/io/FileReader.h"
#include "common/case_insensitive_filesystem.h"
#include "common/io/FileWriter.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "fileformats/sfc/SFCFile.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"

#include <fmt/core.h>
#include <map>
#include <string_view>
#include <algorithm>
#include <fstream>
#include <vector>
#include <system_error>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t CREATURE_TIME_TICKS = 1200; // 2 minutes (Q30)
constexpr uint32_t AUTOSAVE_TICKS = 6000; // 10 minutes (Q10)
constexpr size_t CHEM_GLYCOGEN = 59;

std::string s_world_file_name = "World.openc1.sfc";
bool s_autosave_enabled = true;
std::string s_autosave_error;
uint32_t s_ticks_since_autosave = 0; // counted from loading, so a world isn't saved on its first tick

fs::path game_folder() {
	return g_engine_context.paths->find_path(PATH_TYPE_MAIN, "");
}

bool is_original_world(const std::string& file) {
	const auto lower = to_ascii_lowercase(fs::path(file).filename().string());
	return lower == "eden.sfc" || lower == "world.sfc";
}

bool is_autosave(const std::string& file) {
	return file.find(".autosave.") != std::string::npos;
}

// The files that go with a world file, by name: its graveyard and family (by stem), its
// wolfling record, and its add-on allowances and registrations (by the whole name)
bool is_companion_of(const std::string& world_file, const std::string& name) {
	const auto stem = fs::path(world_file).stem().string();
	if (name == stem + ".graveyard.txt" || name == stem + ".family.txt" || name == world_file + ".wolfling.txt") {
		return true;
	}
	const auto ext = fs::path(name).extension().string();
	return name.rfind(world_file + ".", 0) == 0 && (ext == ".registration" || ext == ".allowance");
}

std::string companion_for(const std::string& old, const std::string& valid, const std::string& name) {
	const auto old_stem = fs::path(old).stem().string(), new_stem = fs::path(valid).stem().string();
	if (name.rfind(old + ".", 0) == 0) {
		return valid + name.substr(old.size());
	}
	return new_stem + name.substr(old_stem.size());
}

// Save As (and the autosaves) take the world's companions along: from the old world's folder
// to the new one's, renamed for the new world file.
void copy_world_companions(const fs::path& from, const fs::path& to, const std::string& old, const std::string& valid) {
	if (valid == old || !fs::is_directory(from)) {
		return;
	}
	std::vector<fs::directory_entry> entries;
	for (const auto& entry : fs::directory_iterator(from)) entries.push_back(entry);
	for (const auto& entry : entries) {
		if (!entry.is_regular_file()) continue;
		const auto name = entry.path().filename().string();
		// the wolfling record is written by save_world itself
		if (!is_companion_of(old, name) || name == old + ".wolfling.txt") continue;
		FileReader in(entry.path());
		auto data = in.read_to_end();
		fs::create_directories(to);
		write_persistent_file(to / companion_for(old, valid, name), data.data(), data.size());
	}
}

void autosave() {
	try {
		const auto base = world_base_name(s_world_file_name);
		const auto folder = world_folder(base + ".autosave.1.sfc");
		// Never replace a manual save during automatic recovery. Three rolling slots
		// stay in the world's Autosaves folder.
		for (int slot = 3; slot > 1; --slot) {
			auto from = folder / fmt::format("{}.autosave.{}.sfc", base, slot - 1);
			auto to = folder / fmt::format("{}.autosave.{}.sfc", base, slot);
			if (fs::is_regular_file(from)) {
				copy_world_companions(folder, folder, from.filename().string(), to.filename().string());
				FileReader in(from);
				auto bytes = in.read_to_end();
				write_persistent_file(to, bytes.data(), bytes.size());
			}
		}
		save_world(base + ".autosave.1.sfc");
		fmt::print("* Recovery autosave for {} complete\n", s_world_file_name);
	} catch (const std::exception& e) {
		s_autosave_error = fmt::format("Recovery save failed: {}", e.what());
		fmt::print(stderr, "* Autosave failed: {}\n", e.what());
	}
}

} // namespace

void world_systems_tick() {
	auto& world = *g_engine_context.world;
	if (world.tick % CREATURE_TIME_TICKS == 0) {
		world.breeders_score += static_cast<uint32_t>(count_living_creatures());
	}
	if (++s_ticks_since_autosave >= AUTOSAVE_TICKS) {
		s_ticks_since_autosave = 0;
		if (s_autosave_enabled) {
			autosave();
		}
	}
}

int32_t creature_health_percent(const Creature& creature) {
	if (creature.dead || !creature.biochemistry) {
		return 0;
	}
	return creature.biochemistry->chemicals[CHEM_GLYCOGEN].concentration * 100 / 255;
}

int32_t creature_gestation_stage(const Creature& c) {
    if (c.sex != 2 || c.zygote.empty() || !c.biochemistry) return 0;
    return (c.biochemistry->chemicals[66].concentration + 31) / 32;
}

uint32_t player_score() {
	const auto& w = *g_engine_context.world;
	return static_cast<uint32_t>(std::min<uint64_t>(99999, uint64_t{256} * w.natural_eggs + w.breeders_score));
}

int32_t count_living_creatures() {
	int32_t living = 0;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* creature = obj->as_creature(); creature && !creature->dead) {
			living++;
		}
	}
	return living;
}

void set_world_file_name(const std::string& name) {
	s_world_file_name = name;
	s_ticks_since_autosave = 0;
}

const std::string& get_world_file_name() {
	return s_world_file_name;
}

std::string world_save_name(std::string name) {
	while (!name.empty() && name.back() == ' ') name.pop_back();
	while (!name.empty() && name.front() == ' ') name.erase(name.begin());
	if (name.empty() || name.size() > 120 || name == "." || name == ".." ||
		name.find_first_of("/\\:\n\r\t") != std::string::npos || name.front() == '.')
		throw Exception("Choose a world name without folders or special characters.");
	if (to_ascii_lowercase(fs::path(name).extension().string()) != ".sfc") name += ".sfc";
	const auto lower = to_ascii_lowercase(name);
	if (lower == "eden.sfc" || lower == "world.sfc")
		throw Exception("Eden and World are original templates. Choose a new save name.");
	return name;
}

void save_world(const std::string& file_name) {
	const auto valid = world_save_name(file_name);
	const auto folder = game_folder();
	const auto destination = world_folder(valid) / valid;
	fs::create_directories(destination.parent_path());
	const auto old = get_world_file_name();
	const auto old_folder = fs::is_directory(world_folder(old)) ? world_folder(old) : folder;
	copy_world_companions(old_folder, world_folder(valid), old, valid);
	auto sfc = sfc_dump_everything();
	VectorWriter out;
	sfc::write_sfc_v1_file(out, sfc);
	const auto& bytes = out.vector();
	write_persistent_file(destination, bytes.data(), bytes.size());
	Wolfling::save_for_world(valid);
	if (valid.find(".autosave.") == std::string::npos) {
		write_persistent_file(folder / "openc1-continue.txt", valid + "\n");
		if (auto profile = session_profile(); !profile.empty())
			write_persistent_file(profile / "active-world.txt", valid + "\n");
	}
}

std::string last_saved_world() {
	const auto folder = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "");
	std::ifstream in(folder / "openc1-continue.txt");
	std::string name;
	if (!std::getline(in, name)) return {};
	try { name = world_save_name(name); } catch (...) { return {}; }
	return find_world(name).empty() ? std::string{} : name;
}

std::string world_base_name(const std::string& world_file) {
	const auto stem = fs::path(world_file).stem().string();
	const auto at = stem.find(".autosave.");
	return at == std::string::npos ? stem : stem.substr(0, at);
}

fs::path world_folder(const std::string& world_file) {
	if (is_original_world(world_file)) {
		return game_folder();
	}
	const auto folder = game_folder() / "Worlds" / world_base_name(world_file);
	return is_autosave(world_file) ? folder / "Autosaves" : folder;
}

fs::path current_world_folder() {
	return game_folder() / "Worlds" / world_base_name(s_world_file_name);
}

fs::path find_world(const std::string& world_file) {
	if (world_file.empty()) {
		return {};
	}
	if (!is_original_world(world_file)) {
		const auto path = world_folder(world_file) / world_file;
		if (fs::is_regular_file(path)) {
			return path;
		}
	}
	return g_engine_context.paths->find_path(PATH_TYPE_MAIN, world_file);
}

std::vector<std::string> list_world_files() {
	std::vector<std::string> files;
	for (auto& f : g_engine_context.paths->find_path_wildcard(PATH_TYPE_MAIN, "*.sfc")) {
		files.push_back(f.filename().string());
	}
	std::error_code err;
	const auto worlds = game_folder() / "Worlds";
	if (fs::is_directory(worlds, err)) {
		for (const auto& world : fs::directory_iterator(worlds, err)) {
			if (!world.is_directory()) continue;
			for (const auto& folder : {world.path(), world.path() / "Autosaves"}) {
				if (!fs::is_directory(folder, err)) continue;
				for (const auto& entry : fs::directory_iterator(folder, err)) {
					if (entry.is_regular_file() && to_ascii_lowercase(entry.path().extension().string()) == ".sfc") {
						files.push_back(entry.path().filename().string());
					}
				}
			}
		}
	}
	std::sort(files.begin(), files.end());
	files.erase(std::unique(files.begin(), files.end()), files.end());
	return files;
}

namespace {

// Moves a file or folder, copying it to the backup first; never replaces anything
bool move_with_backup(const fs::path& from, const fs::path& to, const fs::path& backup) {
	std::error_code err;
	if (fs::exists(to, err)) {
		fmt::print(stderr, "* Not moving {}: {} is already there\n", from.string(), to.string());
		return false;
	}
	if (!fs::exists(backup, err)) {
		fs::create_directories(backup.parent_path(), err);
		fs::copy(from, backup, fs::copy_options::recursive, err);
		if (err) {
			fmt::print(stderr, "* Not moving {}: couldn't back it up ({})\n", from.string(), err.message());
			return false;
		}
	}
	fs::create_directories(to.parent_path(), err);
	fs::rename(from, to, err);
	if (err) {
		// another volume: copy, then remove the original only if the copy is whole
		err.clear();
		fs::copy(from, to, fs::copy_options::recursive, err);
		if (err || (fs::is_regular_file(from) && fs::file_size(from) != fs::file_size(to))) {
			fmt::print(stderr, "* Couldn't move {}: {}\n", from.string(), err.message());
			return false;
		}
		fs::remove_all(from, err);
	}
	return true;
}

} // namespace

int organise_world_files() {
	const auto main = game_folder();
	std::error_code err;
	if (main.empty() || !fs::is_directory(main, err)) {
		return 0;
	}
	const auto backup = main / "Backup" / "Before reorganising";
	std::vector<std::string> names;
	for (const auto& entry : fs::directory_iterator(main, err)) {
		if (entry.is_regular_file()) names.push_back(entry.path().filename().string());
	}
	int moved = 0;
	const auto move = [&](const fs::path& from, const fs::path& to) {
		moved += move_with_backup(from, to, backup / fs::relative(from, main, err)) ? 1 : 0;
	};
	// which world each Norn belongs to, from the worlds' family registries, for the photos
	std::map<std::string, std::string> owner;
	const auto read_family = [&](const fs::path& file, const std::string& base) {
		std::ifstream in(file);
		for (std::string line; std::getline(in, line);) {
			if (line.rfind("norn|", 0) == 0) {
				owner.emplace(line.substr(5, line.find('|', 5) - 5), base);
			}
		}
	};
	std::vector<std::string> worlds;
	for (const auto& name : names) {
		if (to_ascii_lowercase(fs::path(name).extension().string()) == ".sfc" && !is_original_world(name)) {
			worlds.push_back(name);
		}
	}
	for (const auto& name : names) {
		for (const auto& world : worlds) {
			if (!is_autosave(world) && name == fs::path(world).stem().string() + ".family.txt") {
				read_family(main / name, world_base_name(world));
			}
		}
	}
	if (fs::is_directory(main / "Worlds", err)) {
		for (const auto& world : fs::directory_iterator(main / "Worlds", err)) {
			const auto family = world.path() / (world.path().filename().string() + ".family.txt");
			if (fs::is_regular_file(family, err)) read_family(family, world.path().filename().string());
		}
	}
	// the worlds and everything named after them
	for (const auto& world : worlds) {
		move(main / world, world_folder(world) / world);
		for (const auto& name : names) {
			if (is_companion_of(world, name) && fs::exists(main / name, err)) {
				move(main / name, world_folder(world) / name);
			}
		}
		for (const char* kept : {"Wolfling Snapshots", "Wolfling Champions"}) {
			const auto folder = main / kept / world;
			if (!is_autosave(world) && fs::is_directory(folder, err)) {
				move(folder, world_folder(world) / kept);
			}
		}
	}
	// photo albums to the world whose family has the Norn, the rest to Photo Albums; exported
	// creatures to Norns
	for (const auto& name : names) {
		const auto lower = to_ascii_lowercase(name);
		if (!fs::exists(main / name, err)) continue;
		constexpr std::string_view album = ".openc1.photos";
		if (lower.size() > album.size() && lower.compare(lower.size() - album.size(), album.size(), album) == 0) {
			const auto moniker = name.substr(0, name.size() - album.size());
			const auto it = owner.find(moniker);
			move(main / name, (it != owner.end() ? main / "Worlds" / it->second / "Photos" : main / "Photo Albums") / name);
		} else if (lower.size() > 15 && lower.compare(4, 7, " photo ") == 0 && fs::path(lower).extension() == ".bmp") {
			// a photo the Owner's Kit saved as a picture: "3BUK photo 1.bmp"
			const auto it = owner.find(name.substr(0, 4));
			move(main / name, (it != owner.end() ? main / "Worlds" / it->second / "Photos" : main / "Photo Albums") / name);
		} else if (to_ascii_lowercase(fs::path(name).extension().string()) == ".exp") {
			move(main / name, main / "Norns" / name);
		}
	}
	if (moved) {
		fmt::print("* Organised {} files into world folders (copies in {})\n", moved, backup.string());
	}
	return moved;
}

void set_autosave_enabled(bool enabled) {
	s_autosave_enabled = enabled;
}

bool autosave_enabled() {
	return s_autosave_enabled;
}

std::string take_autosave_error() { auto message = std::move(s_autosave_error); s_autosave_error.clear(); return message; }
