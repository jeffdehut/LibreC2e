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

void copy_world_companions(const fs::path& folder, const std::string& old, const std::string& valid) {
	if (valid != old) {
		const auto grave = fs::path(old).stem().string() + ".graveyard.txt";
		const auto family = fs::path(old).stem().string() + ".family.txt";
		std::vector<fs::directory_entry> entries;
		for (const auto& entry : fs::directory_iterator(folder)) entries.push_back(entry);
		for (const auto& entry : entries) {
			if (!entry.is_regular_file()) continue;
			const auto name = entry.path().filename().string();
			std::string target;
			if (name.rfind(old + ".", 0) == 0 &&
				(entry.path().extension() == ".registration" || entry.path().extension() == ".allowance"))
				target = valid + name.substr(old.size());
			else if (name == grave) target = fs::path(valid).stem().string() + ".graveyard.txt";
			else if (name == family) target = fs::path(valid).stem().string() + ".family.txt";
			if (!target.empty()) {
				FileReader in(entry.path());
				auto data = in.read_to_end();
				write_persistent_file(folder / target, data.data(), data.size());
			}
		}
	}
}

void autosave() {
	try {
		const auto base = fs::path(s_world_file_name).stem().string();
		const auto folder = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "");
		// Never replace a manual save during automatic recovery. Three rolling slots
		// stay beside this world so genomes and generated art keep their association.
		for (int slot = 3; slot > 1; --slot) {
			auto from = folder / fmt::format("{}.autosave.{}.sfc", base, slot - 1);
			auto to = folder / fmt::format("{}.autosave.{}.sfc", base, slot);
			if (fs::is_regular_file(from)) {
				copy_world_companions(folder, from.filename().string(), to.filename().string());
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
	const auto folder = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "");
	auto destination = g_engine_context.paths->find_path(PATH_TYPE_MAIN, valid);
	if (destination.empty()) destination = folder / valid;
	copy_world_companions(folder, get_world_file_name(), valid);
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
	return g_engine_context.paths->find_path(PATH_TYPE_MAIN, name).empty() ? std::string{} : name;
}

void set_autosave_enabled(bool enabled) {
	s_autosave_enabled = enabled;
}

bool autosave_enabled() {
	return s_autosave_enabled;
}

std::string take_autosave_error() { auto message = std::move(s_autosave_error); s_autosave_error.clear(); return message; }
