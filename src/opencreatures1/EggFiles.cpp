#include "EggFiles.h"

#include "../../third_party/librecreatures/C1EggFile.h"
#include "CreatureGenetics.h"
#include "EngineContext.h"
#include "Macro.h"
#include "MacroManager.h"
#include "PathManager.h"
#include "PersistentFile.h"
#include "PointerManager.h"
#include "WorldState.h"
#include "WorldSystems.h"
#include "common/Ascii.h"
#include "common/Exception.h"
#include "common/Random.h"
#include "common/io/FileReader.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"

#include <fmt/core.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using openc1::librecreatures::EggFile;

namespace EggFiles {

namespace {

std::function<int(const std::string&)> s_generation_of;
std::function<std::string(const std::string&)> s_name_of;

// the archive's index: one line an egg, "moniker|generation|mother|father|file" (the file
// relative to the Eggs folder)
struct Entry {
	int generation = 0;
	std::string mother, father, file;
};
std::map<std::string, Entry> s_index;
uint64_t s_index_generation = ~0ull;
std::set<std::string> s_imported; // eggs that came from a file this session

fs::path eggs_folder() {
	return current_world_folder() / "Eggs";
}

void load_index() {
	if (s_index_generation == g_engine_context.generation()) {
		return;
	}
	s_index_generation = g_engine_context.generation();
	s_index.clear();
	std::ifstream in(eggs_folder() / "eggs.txt");
	for (std::string line; std::getline(in, line);) {
		std::istringstream fields(line);
		std::string moniker, generation;
		Entry e;
		if (std::getline(fields, moniker, '|') && std::getline(fields, generation, '|') && std::getline(fields, e.mother, '|') &&
			std::getline(fields, e.father, '|') && std::getline(fields, e.file)) {
			e.generation = std::atoi(generation.c_str());
			s_index[moniker] = e;
		}
	}
}

void save_index() {
	std::string text;
	for (const auto& [moniker, e] : s_index) {
		text += fmt::format("{}|{}|{}|{}|{}\n", moniker, e.generation, e.mother, e.father, e.file);
	}
	write_persistent_file(eggs_folder() / "eggs.txt", text);
}

// the shipped Hatchery and starting genomes are nobody's children
bool is_founder(const std::string& m) {
	return m.empty() || m == "EVE_" || m == "ADAM" || m.rfind("mum", 0) == 0 || m.rfind("dad", 0) == 0;
}

std::string name_of(const std::string& moniker) {
	const std::string name = s_name_of ? s_name_of(moniker) : std::string();
	return name.empty() ? moniker : name;
}

// a name that's safe in a file name on every system
std::string file_safe(std::string text) {
	for (char& c : text) {
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 32) {
			c = '_';
		}
	}
	return text;
}

std::vector<uint8_t> genome_file(const std::string& moniker) {
	const auto path = g_engine_context.paths->find_path(PATH_TYPE_GENETICS, moniker + ".gen");
	if (path.empty()) {
		return {};
	}
	FileReader in(path);
	return in.read_to_end();
}

bool moniker_in_use(const std::string& moniker) {
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature(); c && c->moniker == moniker) {
			return true;
		}
		if (obj->family == 2 && obj->genus == 5 && moniker_from_integer(obj->obv0) == moniker) {
			return true;
		}
	}
	return false;
}

bool is_egg(const Object& obj) {
	// 2 5 6 is the hidden Grendel mother, not an egg
	return obj.family == 2 && obj.genus == 5 && obj.species != 6;
}

void archive(const Object& egg) {
	const std::string moniker = moniker_from_integer(egg.obv0);
	if (moniker.size() != 4 || s_index.count(moniker)) {
		return;
	}
	auto bytes = egg_bytes(moniker, static_cast<uint32_t>(std::clamp(egg.obv1, 0, 2)));
	if (bytes.empty()) {
		return; // its genome isn't made yet, or is gone
	}
	Entry e;
	std::string folder;
	if (const auto* parents = genome_parents(moniker)) {
		e.mother = parents->first;
		e.father = parents->second;
		const auto parent_generation = [](const std::string& m) { return is_founder(m) ? 0 : std::max(1, generation(m)); };
		e.generation = 1 + std::max(parent_generation(e.mother), parent_generation(e.father));
		folder = fmt::format("Generation {}", e.generation);
	} else {
		folder = s_imported.count(moniker) ? "Imported" : "Generation unknown";
	}
	std::string label = moniker;
	if (!e.mother.empty()) {
		label += e.father.empty() || e.father == e.mother ? fmt::format(" (child of {})", name_of(e.mother))
														  : fmt::format(" ({} and {})", name_of(e.mother), name_of(e.father));
	}
	e.file = folder + "/" + file_safe(label) + ".egg";
	write_persistent_file(eggs_folder() / e.file, bytes.data(), bytes.size());
	s_index[moniker] = e;
	save_index();
	fmt::print("* Egg {} saved to {}\n", moniker, (eggs_folder() / e.file).string());
}

} // namespace

bool is_egg_file(const fs::path& path) {
	return to_ascii_lowercase(path.extension().string()) == ".egg";
}

std::vector<uint8_t> egg_bytes(const std::string& moniker, uint32_t sex) {
	EggFile egg;
	egg.genome = genome_file(moniker);
	if (egg.genome.size() < 8 || moniker.size() != 4) {
		return {};
	}
	egg.moniker = moniker;
	egg.sex = sex;
	auto bytes = egg.bytes();
	// every file written must read back
	EggFile check;
	return check.read(bytes.data(), bytes.size()) ? bytes : std::vector<uint8_t>{};
}

void save_copy(const std::string& moniker, uint32_t sex, const fs::path& to) {
	std::vector<uint8_t> bytes;
	if (const auto kept = archived(moniker); !kept.empty()) {
		FileReader in(kept);
		bytes = in.read_to_end();
	}
	if (bytes.empty()) {
		bytes = egg_bytes(moniker, sex);
	}
	if (bytes.empty()) {
		throw Exception(fmt::format("There's no egg or genome for {}", moniker));
	}
	write_persistent_file(to, bytes.data(), bytes.size());
}

Object* held_egg() {
	auto* obj = g_engine_context.objects->try_get(g_engine_context.pointer->m_carried_object);
	return obj && is_egg(*obj) ? obj : nullptr;
}

void export_held_egg(const fs::path& to) {
	auto* egg = held_egg();
	if (!egg) {
		throw Exception("The hand isn't holding an egg");
	}
	const auto bytes = egg_bytes(moniker_from_integer(egg->obv0), static_cast<uint32_t>(std::clamp(egg->obv1, 0, 2)));
	if (bytes.empty()) {
		throw Exception("This egg's genome is missing");
	}
	load_index();
	archive(*egg); // the world keeps its record of it
	write_persistent_file(to, bytes.data(), bytes.size());
	// it leaves the world, as an exported creature does, and the hand lets go
	g_engine_context.pointer->m_carried_object = {};
	g_engine_context.objects->destroy_later(egg->uid);
}

std::string import_egg(const fs::path& from) {
	std::vector<uint8_t> bytes;
	{
		std::error_code err;
		const auto size = fs::file_size(from, err);
		if (err || size > EggFile::HEADER_BYTES + EggFile::MAX_GENOME_BYTES) {
			throw Exception("This isn't an egg file");
		}
		FileReader in(from);
		bytes = in.read_to_end();
	}
	EggFile egg;
	if (!egg.read(bytes.data(), bytes.size())) {
		throw Exception("This isn't an egg file, or it's damaged");
	}
	// the same genes as a new individual if this world already uses the name
	std::string moniker = egg.moniker;
	const auto existing = genome_file(moniker);
	if (moniker_in_use(moniker) || (!existing.empty() && existing != egg.genome)) {
		moniker = new_moniker();
	}
	{
		auto out = g_engine_context.paths->create_file(PATH_TYPE_GENETICS, moniker + ".gen");
		out.write(reinterpret_cast<const char*>(egg.genome.data()), egg.genome.size());
		out.close();
	}
	s_imported.insert(moniker);
	// a fresh egg (eggs.spr, 8 frames a slot) set down by the incubator, the camera with it
	const int slot = rand_int32(0, 5);
	Macro m(fmt::format("inst,sys: cmra 2223 724,new: simp eggs 8 {} 2000 0,pose 3,setv clas {},setv attr 67,"
						"setv obv0 tokn {},setv obv1 {},tick 2400,mvto {} 870,endm",
		8 * slot, static_cast<int32_t>(egg.classifier & 0xffffff00u), moniker, egg.sex, 2408 + 40 * slot));
	m.ownr = m.targ = g_engine_context.pointer->m_pointer_tool;
	g_engine_context.macros->add(m);
	// used up, unless it's a world's own copy
	bool kept = false;
	for (auto p = from.parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path()) {
		kept = kept || (p.filename() == "Eggs" && p.parent_path().parent_path().filename() == "Worlds");
	}
	if (!kept) {
		std::error_code err;
		fs::remove(from, err);
	}
	return moniker;
}

void set_lookups(std::function<int(const std::string&)> generation, std::function<std::string(const std::string&)> name) {
	s_generation_of = std::move(generation);
	s_name_of = std::move(name);
}

int generation(const std::string& moniker) {
	if (const int g = s_generation_of ? s_generation_of(moniker) : 0; g > 0) {
		return g;
	}
	load_index();
	auto it = s_index.find(moniker);
	return it == s_index.end() ? 0 : it->second.generation;
}

fs::path archived(const std::string& moniker) {
	load_index();
	auto it = s_index.find(moniker);
	if (it == s_index.end()) {
		return {};
	}
	const auto path = eggs_folder() / it->second.file;
	return fs::is_regular_file(path) ? path : fs::path{};
}

void tick() {
	// a few times a second is soon enough: an egg's genome is made the tick it's laid
	if (g_engine_context.world->tick % 8 != 0) {
		return;
	}
	load_index();
	for (auto* obj : *g_engine_context.objects) {
		if (is_egg(*obj)) {
			try {
				archive(*obj);
			} catch (const std::exception& e) {
				fmt::print(stderr, "* Couldn't save egg {}: {}\n", moniker_from_integer(obj->obv0), e.what());
			}
		}
	}
}

void world_changed() {
	s_index_generation = ~0ull;
}

} // namespace EggFiles
