#include "Wolfling.h"

#include "EngineContext.h"
#include "Macro.h"
#include "MacroManager.h"
#include "MapManager.h"
#include "PathManager.h"
#include "PointerManager.h"
#include "SaveAndShare.h"
#include "WorldState.h"
#include "WorldSystems.h"
#include "common/Random.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <cstdlib>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <imgui.h>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace Wolfling {

namespace {

constexpr uint32_t TICKS_PER_HOUR = 36000;
constexpr uint32_t CHECK_EVERY = 300; // births, deaths, names, the population floor
constexpr uint32_t RESTOCK_EVERY = 3000;

struct Event {
	uint32_t tick;
	std::string text;
};

struct Spot {
	float x, y;
	bool floating;
};

struct Run {
	bool active = false;
	Settings settings;
	uint32_t started = 0;
	int number = 1; // which run in this game folder, for the snapshot folder
	std::map<std::string, int> generations; // moniker -> generation
	std::set<std::string> dead; // monikers already recorded as dead
	std::set<std::string> names; // every name the run has given, never given again
	std::vector<Spot> food_spots;
	std::vector<int> population; // living Norns, each game hour
	std::vector<Event> events;
	int births = 0, deaths = 0, eggs_added = 0;
	std::map<std::string, int> death_causes;
	uint32_t total_lifespan_hours = 0;
};

Run s_run;
bool s_show_sheet = false;
bool s_show_dashboard = false;
Settings s_sheet; // the start sheet's choices

fs::path game_folder() {
	return g_engine_context.paths->find_path(PATH_TYPE_MAIN, "Creatures.exe").parent_path();
}

// beside the world file: in its folder, or beside a snapshot given by its whole path
fs::path run_file(const std::string& world_file) {
	if (fs::path(world_file).is_absolute()) {
		return fs::path(world_file + ".wolfling.txt");
	}
	return world_folder(world_file) / (world_file + ".wolfling.txt");
}

std::string label(const Creature& c) {
	return c.history_name.empty() || c.history_name == "<Un Named>" ? c.moniker : c.history_name;
}

double hours(uint32_t ticks) {
	return static_cast<double>(ticks) / TICKS_PER_HOUR;
}

// ticks since the run started; 0 if the world's clock is behind the run's start (a run saved
// with another copy of the world), rather than wrapping round to years
uint32_t run_ticks() {
	return g_engine_context.world->tick >= s_run.started ? g_engine_context.world->tick - s_run.started : 0;
}

void note(std::string text) {
	fmt::print("INFO [Wolfling] {}\n", text);
	s_run.events.push_back({g_engine_context.world->tick, std::move(text)});
	if (s_run.events.size() > 500) {
		s_run.events.erase(s_run.events.begin());
	}
}

// --- saving the run alongside its world ---

void write_run(const std::string& world_file) {
	std::error_code err;
	fs::create_directories(run_file(world_file).parent_path(), err); // a world's folder may be new
	std::ofstream out(run_file(world_file));
	out << "active " << s_run.active << "\n";
	out << "settings " << s_run.settings.variant << " " << s_run.settings.food << " " << s_run.settings.population_floor << " "
		<< s_run.settings.names_by_generation << " " << s_run.settings.snapshot_hours << "\n";
	out << "started " << s_run.started << " number " << s_run.number << "\n";
	out << "counts " << s_run.births << " " << s_run.deaths << " " << s_run.eggs_added << " " << s_run.total_lifespan_hours << "\n";
	for (auto& [m, g] : s_run.generations) {
		out << "generation " << m << " " << g << "\n";
	}
	for (auto& m : s_run.dead) {
		out << "dead " << m << "\n";
	}
	for (auto& n : s_run.names) {
		out << "name " << n << "\n";
	}
	for (auto& s : s_run.food_spots) {
		out << "spot " << s.x << " " << s.y << " " << s.floating << "\n";
	}
	for (int p : s_run.population) {
		out << "population " << p << "\n";
	}
	for (auto& [cause, n] : s_run.death_causes) {
		out << "cause " << n << " " << cause << "\n";
	}
	for (auto& e : s_run.events) {
		out << "event " << e.tick << " " << e.text << "\n";
	}
}

void save() {
	write_run(get_world_file_name());
}

void load() {
	s_run = Run{};
	// The old folder-wide file has no world identity; applying it to any world would
	// recreate the cross-world leak. Leave it intact, but only restore scoped files.
	std::ifstream in(run_file(get_world_file_name()));
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream s(line);
		std::string key;
		s >> key;
		if (key == "active") {
			s >> s_run.active;
		} else if (key == "settings") {
			int v = 0, f = 0;
			s >> v >> f >> s_run.settings.population_floor >> s_run.settings.names_by_generation >> s_run.settings.snapshot_hours;
			s_run.settings.variant = static_cast<Variant>(std::clamp(v, 0, 2));
			s_run.settings.food = static_cast<Food>(std::clamp(f, 0, 2));
		} else if (key == "started") {
			std::string word;
			s >> s_run.started >> word >> s_run.number;
		} else if (key == "counts") {
			s >> s_run.births >> s_run.deaths >> s_run.eggs_added >> s_run.total_lifespan_hours;
		} else if (key == "generation") {
			std::string m;
			int g = 0;
			s >> m >> g;
			s_run.generations[m] = g;
		} else if (key == "dead") {
			std::string m;
			s >> m;
			s_run.dead.insert(m);
		} else if (key == "name") {
			std::string n;
			std::getline(s >> std::ws, n);
			s_run.names.insert(n);
		} else if (key == "spot") {
			Spot spot{};
			s >> spot.x >> spot.y >> spot.floating;
			s_run.food_spots.push_back(spot);
		} else if (key == "population") {
			int p = 0;
			s >> p;
			s_run.population.push_back(p);
		} else if (key == "cause") {
			int n = 0;
			s >> n;
			std::string cause;
			std::getline(s >> std::ws, cause);
			s_run.death_causes[cause] = n;
		} else if (key == "event") {
			Event e{};
			s >> e.tick;
			std::getline(s >> std::ws, e.text);
			s_run.events.push_back(e);
		}
	}
}

// --- generations and names ---

int generation_of(const Creature& c) {
	auto it = s_run.generations.find(c.moniker);
	if (it != s_run.generations.end()) {
		return it->second;
	}
	// a child is one generation more than its higher-generation parent; Hatchery eggs (whose
	// parents are the mum and dad genomes) and Norns of unknown parents are generation 1
	auto parent = [](const std::string& m) {
		auto p = s_run.generations.find(m);
		return p != s_run.generations.end() ? p->second : 0;
	};
	const int g = std::max(parent(c.mother), parent(c.father)) + 1;
	s_run.generations[c.moniker] = g;
	return g;
}

// Names made from syllables and word parts, so that they practically never repeat, even
// between players. A given name starts with the generation's letter (the community's
// convention: G1 names start with A, G2 with B, ...), and is followed by a family name:
// a Hatchery Norn gets a new one, and a Norn born in the world takes its mother's (or its
// father's). About 270,000 given names for each letter and about 2,000 family names. A name
// already given in this game folder (the family registry, the run's own list, anyone in the
// world) is never given again.
namespace naming {

const char* const CLUSTERS[] = {"br", "bl", "cl", "cr", "dr", "fl", "fr", "gl", "gr", "pl", "pr", "sh", "st", "th", "tr", "ch", "sl", "sn"};
const char* const SOFT[] = {"l", "m", "n", "r", "s", "v", "d", "t", "b", "f", "w", "z"}; // between syllables
const char* const VOWELS[] = {"a", "e", "i", "o", "u", "a", "e", "i", "o", "ai", "ea", "ie", "oa", "ou", "y"};
const char* const SIMPLE[] = {"a", "e", "i", "o", "u"};
const char* const CODAS[] = {"n", "l", "r", "s", "m", "th", "nd", "rn", "ll"};
const char* const ENDINGS[] = {"a", "o", "ie", "y", "en", "in", "el", "is", "ra", "ia", "on", "us", "eth", "wyn", "ley", "by", "ix",
	"elle", "ina", "ora", "an", "et", "ise", "ith", "ette", "ani"};
const char* const FAMILY_FRONT[] = {"Ash", "Alder", "Amber", "Bram", "Briar", "Brook", "Bracken", "Cliff", "Clover", "Copper", "Dun", "Elder",
	"Elm", "Fen", "Fern", "Fox", "Glen", "Gorse", "Hazel", "Heather", "Hollow", "Holly", "Ivy", "Juniper", "Kestrel", "Lark", "Linden",
	"Maple", "Marsh", "Meadow", "Moss", "North", "Oak", "Otter", "Pebble", "Pine", "Quill", "Raven", "Reed", "Rowan", "Rush", "Sage",
	"Sedge", "Sorrel", "Stone", "Sunny", "Tansy", "Thistle", "Thorn", "Tumble", "Vale", "Wren", "Willow", "Wild", "Yarrow", "Bramble",
	"Cobble", "Dapple", "Moon", "Star"};
const char* const FAMILY_BACK[] = {"wick", "dale", "brook", "ford", "well", "ley", "more", "by", "field", "wood", "stone", "mere", "holt",
	"combe", "burrow", "croft", "fell", "worth", "ridge", "hill", "bank", "shaw", "dell", "glade", "hurst", "moor", "fold", "hollow",
	"vale", "down", "wyn", "low"};
// never inside a name
const char* const BLOCKED[] = {"ass", "fuk", "fuc", "shit", "cunt", "dick", "cock", "piss", "slut", "whor", "nazi", "rape", "anus", "tits", "fag", "nig", "kik", "spic", "cum", "poo", "wank"};

template <typename T, size_t N>
const char* pick(const T (&list)[N], std::mt19937_64& rng) {
	return list[std::uniform_int_distribution<size_t>(0, N - 1)(rng)];
}

bool chance(std::mt19937_64& rng, int in) {
	return std::uniform_int_distribution<int>(1, in)(rng) == 1;
}

bool is_vowel(char c) {
	return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
}

std::string given(char letter, std::mt19937_64& rng) {
	std::string name;
	// the first syllable starts with the generation's letter
	if (letter == 'a' || letter == 'e' || letter == 'i' || letter == 'o' || letter == 'u') {
		name += letter;
		name += pick(CODAS, rng);
	} else {
		std::vector<std::string> clusters;
		for (const char* c : CLUSTERS) {
			if (c[0] == letter) {
				clusters.emplace_back(c);
			}
		}
		if (letter == 'q') {
			name += "qu";
		} else if (!clusters.empty() && chance(rng, 3)) {
			name += clusters[std::uniform_int_distribution<size_t>(0, clusters.size() - 1)(rng)];
		} else {
			name += letter;
		}
		std::string vowel = chance(rng, 3) ? pick(VOWELS, rng) : pick(SIMPLE, rng);
		while ((letter == 'q' && vowel[0] == 'u') || (letter == 'y' && vowel[0] == 'y')) {
			vowel = pick(SIMPLE, rng);
		}
		name += vowel;
		if (chance(rng, 3)) {
			name += pick(CODAS, rng);
		}
	}
	// a soft middle syllable, half the time
	if (chance(rng, 2)) {
		if (!is_vowel(name.back())) {
			name += pick(SIMPLE, rng);
		}
		name += pick(SOFT, rng);
		name += pick(SIMPLE, rng);
	}
	// the ending: straight on after a vowel or a consonant, with a joining letter if needed
	const std::string ending = pick(ENDINGS, rng);
	const bool ends_vowel = is_vowel(name.back()), starts_vowel = is_vowel(ending[0]);
	if (ends_vowel && starts_vowel) {
		name += pick(SOFT, rng);
	} else if (!ends_vowel && !starts_vowel) {
		name += pick(SIMPLE, rng);
	}
	name += ending;
	name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
	return name;
}

std::string family(std::mt19937_64& rng) {
	std::string front = pick(FAMILY_FRONT, rng), back = pick(FAMILY_BACK, rng);
	// no "Valevale" or "Hollowhollow"
	while (front.size() >= back.size() && std::equal(back.begin() + 1, back.end(), front.end() - static_cast<long>(back.size()) + 1)) {
		back = pick(FAMILY_BACK, rng);
	}
	return front + back;
}

bool acceptable(const std::string& name, size_t longest) {
	if (name.size() < 3 || name.size() > longest) {
		return false;
	}
	std::string lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	for (const char* b : BLOCKED) {
		if (lower.find(b) != std::string::npos) {
			return false;
		}
	}
	// no three of the same letter in a row, and no three consonants in a row
	int run = 1, consonants = 0;
	for (size_t i = 0; i < lower.size(); ++i) {
		run = i > 0 && lower[i] == lower[i - 1] ? run + 1 : 1;
		consonants = is_vowel(lower[i]) ? 0 : consonants + 1;
		if (run >= 3 || consonants >= 3) {
			return false;
		}
	}
	return true;
}

} // namespace naming

std::function<std::string(const std::string&)> s_name_of;
std::function<bool(const std::string&)> s_name_taken;

// a family name from a parent's name ("Alvie Mossford" gives "Mossford")
std::string family_of(const std::string& parent_moniker) {
	std::string name;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature(); c && c->moniker == parent_moniker) {
			name = c->history_name;
		}
	}
	if (name.empty() && s_name_of) {
		name = s_name_of(parent_moniker);
	}
	const auto space = name.rfind(' ');
	return space != std::string::npos && space + 1 < name.size() ? name.substr(space + 1) : "";
}

std::string name_for(int generation, const std::string& moniker, const std::string& mother = "", const std::string& father = "") {
	const char letter = static_cast<char>('a' + (std::max(1, generation) - 1) % 26);
	std::set<std::string> used = s_run.names;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature()) {
			used.insert(c->history_name);
		}
	}
	// different on every computer and every time: the system's randomness, with the moniker
	std::random_device device;
	std::seed_seq seed{device(), device(), device(), static_cast<unsigned>(std::hash<std::string>{}(moniker)),
		static_cast<unsigned>(g_engine_context.world->tick)};
	std::mt19937_64 rng(seed);
	std::string surname = family_of(mother);
	if (surname.empty()) {
		surname = family_of(father);
	}
	for (int attempt = 0; attempt < 500; ++attempt) {
		const std::string first = naming::given(letter, rng);
		const std::string last = surname.empty() ? naming::family(rng) : surname;
		const std::string name = first + " " + last;
		if (naming::acceptable(first, 9) && !used.count(name) && !(s_name_taken && s_name_taken(name))) {
			return name;
		}
	}
	return fmt::format("{}{}", static_cast<char>(std::toupper(letter)), moniker); // practically never
}

} // namespace

void set_family_lookup(std::function<std::string(const std::string&)> name_of, std::function<bool(const std::string&)> taken) {
	s_name_of = std::move(name_of);
	s_name_taken = std::move(taken);
}

std::vector<std::string> sample_names(int generation, int n) {
	std::vector<std::string> out;
	const auto saved = s_run.names;
	for (int i = 0; i < n; ++i) {
		out.push_back(name_for(generation, fmt::format("T{:03}", i)));
		s_run.names.insert(out.back());
	}
	s_run.names = saved;
	return out;
}

namespace {

// --- food ---

bool is_cheese(const Object& o) {
	return o.family == 2 && o.genus == 6 && o.species == 1;
}

// the Health Kit's Cheese (batch 5, Q6; class 2 6 1, batch 6)
void make_cheese(const Spot& spot) {
	Macro m(fmt::format("inst,new: simp food 3 3 3500 0,setv clas 33947904,setv attr {},bhvr 0 1,setv obv0 1,mvto {} {},endm",
		spot.floating ? 67 | 32 : 67, static_cast<int>(spot.x), static_cast<int>(spot.y)));
	m.ownr = m.targ = g_engine_context.pointer->m_pointer_tool;
	g_engine_context.macros->add(m);
}

void plan_food_spots() {
	s_run.food_spots.clear();
	// where the world's own cheese is at the start
	for (auto* obj : *g_engine_context.objects) {
		// only cheese lying free in the world (not held, or in a machine)
		if (is_cheese(*obj) && !g_engine_context.objects->try_get(obj->carrier) && obj->movement_status == MOVEMENT_AUTONOMOUS) {
			auto b = obj->get_bbox();
			if (b.y < 1200) {
				s_run.food_spots.push_back({b.x, b.y, false});
			}
		}
	}
	// a row of floating cheese just below the top of each room wide enough to walk in, as
	// players set up with add-ons (a creature can eat what's in its room and in sight, at any
	// height: batch 6, Q7)
	if (s_run.settings.food == FOOD_LINE) {
		for (const auto& room : g_engine_context.map->get_rooms()) {
			if (room.width() < 160 || room.height() < 80) {
				continue;
			}
			for (int x = room.left + 60; x + 24 < room.right - 40; x += 240) {
				s_run.food_spots.push_back({static_cast<float>(x), static_cast<float>(room.top + 8), true});
			}
		}
	}
}

void restock() {
	int added = 0;
	// never more cheese than there are spots, whatever has happened to the cheese put out
	int cheese = 0;
	for (auto* obj : *g_engine_context.objects) {
		if (is_cheese(*obj) && obj->get_bbox().y < 1200) {
			cheese++;
		}
	}
	int room_for = static_cast<int>(s_run.food_spots.size()) - cheese;
	for (const auto& spot : s_run.food_spots) {
		if (room_for <= 0) {
			break;
		}
		bool there = false;
		for (auto* obj : *g_engine_context.objects) {
			if (is_cheese(*obj)) {
				auto b = obj->get_bbox();
				if (std::abs(b.x - spot.x) < 40 && std::abs(b.y - spot.y) < 40) {
					there = true;
					break;
				}
			}
		}
		if (!there) {
			make_cheese(spot);
			added++;
			room_for--;
		}
	}
	// honey pots refill quietly, as if someone had carried them to a hive (batch 6, Q6)
	for (auto* obj : *g_engine_context.objects) {
		if (obj->family == 2 && obj->genus == 6 && obj->species == 2 && obj->obv0 >= 10 && !g_engine_context.objects->try_get(obj->carrier)) {
			obj->obv0 = 0;
			obj->attr = static_cast<uint8_t>(obj->attr & ~ATTR_INVISIBLE);
			if (auto* part = obj->get_part(0)) {
				part->set_pose(0);
			}
		}
	}
	if (added > 0) {
		fmt::print("INFO [Wolfling] restocked {} cheese\n", added);
	}
}

// --- the population floor ---

void add_egg() {
	// a Hatchery egg (batch 4, Q12) of the rarer sex among the living
	int males = 0, females = 0;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature(); c && c->genus == 1 && !c->dead) {
			(c->sex == 2 ? females : males)++;
		}
	}
	const int n = rand_int32(0, 5);
	const bool female = females < males || (females == males && rand_int32(0, 1) == 1);
	Macro m(fmt::format(
		"inst,new: simp eggs 8 {} 2000 0,pose 3,setv clas 33882624,setv attr 67,"
		"new: gene tokn mum{} tokn dad{} obv0,setv obv1 {},tick 2400,mvto {} 870,endm",
		8 * n, n + 1, n + 1, female ? 2 : 1, 2408 + 40 * n));
	m.ownr = m.targ = g_engine_context.pointer->m_pointer_tool;
	g_engine_context.macros->add(m);
	s_run.eggs_added++;
	note(fmt::format("Fewer than {} Norns alive: a {} egg was added", s_run.settings.population_floor, female ? "female" : "male"));
}

// --- what happened ---

void check_creatures() {
	int alive = 0;
	bool egg = false;
	for (auto* obj : *g_engine_context.objects) {
		if (obj->family == 2 && obj->genus == 5 && obj->species == 2) {
			egg = true;
		}
		auto* c = obj->as_creature();
		if (!c || c->genus != 1) {
			continue;
		}
		const bool known = s_run.generations.count(c->moniker) > 0;
		const int g = generation_of(*c);
		if (!known) {
			if (c->age_ticks < CHECK_EVERY * 4 && g_engine_context.world->tick > s_run.started) {
				s_run.births++;
				note(fmt::format("{} was born (generation {})", label(*c), g));
			} else {
				note(fmt::format("{} is in the run (generation {})", label(*c), g));
			}
		}
		if (s_run.settings.names_by_generation && c->history_name == "<Un Named>" && !c->dead) {
			c->history_name = name_for(g, c->moniker, c->mother, c->father);
			s_run.names.insert(c->history_name);
			note(fmt::format("{} was named {}", c->moniker, c->history_name));
		}
		if (c->dead) {
			if (!s_run.dead.count(c->moniker)) {
				s_run.dead.insert(c->moniker);
				s_run.deaths++;
				const uint32_t age_hours = c->age_ticks / TICKS_PER_HOUR;
				s_run.total_lifespan_hours += age_hours;
				// glycogen at or below 8 is the starvation death (batch 6, Q8); it's read a little
				// later, and a dead creature's glycogen drifts up a little, hence the margin
				std::string cause = "other causes";
				if (c->biochemistry && c->biochemistry->chemicals[59].concentration <= 24) {
					cause = "starvation";
				}
				s_run.death_causes[cause]++;
				note(fmt::format("{} died of {}, aged {:.1f} hours (generation {})", label(*c), cause, hours(c->age_ticks), g));
			}
		} else {
			alive++;
		}
	}
	// One egg at a time: wait while the last egg added is still unhatched, but never more than
	// about two game hours, so an egg that can't hatch (stuck somewhere, or the world's own
	// unhatched eggs) doesn't stop the floor for good.
	static uint32_t last_egg_tick = 0;
	const uint32_t now = g_engine_context.world->tick;
	const bool waited_long = now - last_egg_tick > 2 * TICKS_PER_HOUR || now < last_egg_tick;
	if (s_run.settings.population_floor > 0 && alive < s_run.settings.population_floor && (!egg || waited_long)) {
		add_egg();
		last_egg_tick = now;
	}
}

void take_snapshot() {
	if (!autosave_enabled()) {
		return;
	}
	try {
		save_world(get_world_file_name());
		const fs::path folder = current_world_folder() / "Wolfling Snapshots" / fmt::format("Run {}", s_run.number);
		std::error_code err;
		fs::create_directories(folder, err);
		const uint32_t hour = (run_ticks()) / TICKS_PER_HOUR;
		const auto snapshot = folder / fmt::format("hour {:05}.sfc", hour);
		fs::copy_file(find_world(get_world_file_name()), snapshot, fs::copy_options::overwrite_existing, err);
		if (!err) {
			write_run(snapshot.string());
		}
		// keep the 40 latest
		std::vector<fs::path> shots;
		for (auto& e : fs::directory_iterator(folder, err)) {
			if (e.path().extension() == ".sfc") {
				shots.push_back(e.path());
			}
		}
		std::sort(shots.begin(), shots.end());
		for (size_t i = 0; i + 40 < shots.size(); ++i) {
			fs::remove(shots[i], err);
			fs::remove(run_file(shots[i].string()), err);
		}
		note(fmt::format("Snapshot saved: hour {}", hour));
	} catch (std::exception& e) {
		fmt::print("WARN [Wolfling] couldn't save a snapshot: {}\n", e.what());
	}
}

const char* variant_name(Variant v) {
	switch (v) {
		case VARIANT_STRICT: return "Strict";
		case VARIANT_RELAXED: return "Relaxed";
		default: return "Feral";
	}
}

} // namespace

// Not in the original. The shipped world's food doesn't grow with its population (batch 6, Q6:
// cheese never renews; carrots and fruit only recycle a fixed number), and a Norn only notices
// food in its own room within about 500 px (batch 7, Q3), so in a crowd Norns starved even
// with plenty of food elsewhere. Every game minute, each living Norn with no usable food in
// sight gets a cheese on the floor near it, in its own room; at most six a minute, and none
// while the world already has about four items per Norn. Off during a wolfling run with no
// food help. The cheese goes only beside a Norn standing on its own floor (not carried, in
// a vehicle or falling), within a step of its feet, and only where the room lookup for that
// spot is the Norn's own room, so it can't be left in the air, in a gap or out at sea.
void keep_food_for_population() {
	if (s_run.active && s_run.settings.food == FOOD_NONE) {
		return;
	}
	std::vector<Creature*> norns;
	std::vector<Rect2f> food;
	float cheese_height = 24; // the world's own cheese, when there is one
	for (auto* obj : *g_engine_context.objects) {
		if (is_cheese(*obj) && obj->get_part(0)) {
			cheese_height = obj->get_bbox().height;
		}
		if (auto* c = obj->as_creature()) {
			if (c->genus == 1 && !c->dead && c->movement_status == MOVEMENT_AUTONOMOUS && !g_engine_context.objects->try_get(c->carrier)) {
				norns.push_back(c);
			}
			continue;
		}
		if (obj->family != 2 || obj->genus != 6 || g_engine_context.objects->try_get(obj->carrier) || (obj->attr & ATTR_INVISIBLE) || !obj->get_part(0) || obj->get_bbox().y > 1200) {
			continue;
		}
		const int32_t pose = obj->get_part(0)->pose();
		if (obj->species == 1 || (obj->species == 3 && pose >= 5) || obj->species == 4 || (obj->species == 2 && obj->obv0 < 10)) {
			food.push_back(obj->get_bbox());
		}
	}
	if (norns.empty() || food.size() >= norns.size() * 4 + 10) {
		return;
	}
	// the hungriest first (hunger is drive 2)
	std::sort(norns.begin(), norns.end(), [](Creature* a, Creature* b) { return a->loci.drives[2] > b->loci.drives[2]; });
	int added = 0;
	for (Creature* c : norns) {
		if (added >= 6) {
			break;
		}
		const Rect2i room = c->limit;
		const auto* own = g_engine_context.map->find_room_containing(c->ground_x(), c->ground_y() - 1);
		// standing on its floor, in a real room of the visible world
		if (room.width <= 40 || room.bottom() > 1200 || room.x < 0 || std::abs(c->ground_y() - static_cast<float>(room.bottom())) > 4 || !own || own->type == 2) {
			continue;
		}
		const Rect2f r{static_cast<float>(room.x), static_cast<float>(room.y), static_cast<float>(room.width), static_cast<float>(room.height)};
		const bool in_sight = std::any_of(food.begin(), food.end(), [&](const Rect2f& f) {
			const float cx = f.x + f.width / 2;
			return f.right() > r.x && f.x < r.right() && f.bottom() > r.y && f.y < r.bottom() && std::abs(cx - c->ground_x()) < 500;
		});
		if (in_sight) {
			continue;
		}
		const float x = std::clamp(c->ground_x() + static_cast<float>(rand_int32(-60, 60)), static_cast<float>(room.x + 4), static_cast<float>(room.right() - 28));
		const float y = static_cast<float>(room.bottom()) - cheese_height;
		const Rect2i lands = g_engine_context.map->limits_for(x, static_cast<float>(room.bottom()), Rect2i{});
		if (lands.x != room.x || lands.y != room.y || lands.width != room.width || lands.height != room.height) {
			continue;
		}
		make_cheese(Spot{x, y, false});
		if (std::getenv("OPENCREATURES1_TRACE_FOOD")) {
			fmt::print("TRACE [Food] cheese at {:.0f},{:.0f} for {} at {:.0f},{:.0f}; room {} {} {} {} type {}\n", x, y, c->moniker, c->ground_x(), c->ground_y(), room.x, room.y, room.right(), room.bottom(), own->type);
		}
		food.push_back(Rect2f{x, y, 24, cheese_height});
		added++;
	}
	if (added > 0) {
		fmt::print("INFO [Food] {} Norns, {} food items: added {} cheese where Norns had none in sight\n", norns.size(), food.size() - static_cast<size_t>(added), added);
	}
}

bool active() {
	return s_run.active;
}

const Settings& settings() {
	return s_run.settings;
}

void start(const Settings& settings) {
	const int number = s_run.number + (s_run.started ? 1 : 0);
	s_run = Run{};
	s_run.active = true;
	s_run.settings = settings;
	s_run.started = g_engine_context.world->tick;
	s_run.number = number;
	plan_food_spots();
	note(fmt::format("The run started: {}, food help {}, population floor {}", variant_name(settings.variant),
		settings.food == FOOD_NONE ? "off" : settings.food == FOOD_RESTOCK ? "restock" : "restock and a food line", settings.population_floor));
	check_creatures();
	if (settings.food != FOOD_NONE) {
		restock();
	}
	save();
}

void stop() {
	if (!s_run.active) {
		return;
	}
	note("The run ended");
	s_run.active = false;
	save();
}

void tick() {
	if (!s_run.active) {
		return;
	}
	if (g_engine_context.world->tick < s_run.started) {
		s_run.started = g_engine_context.world->tick; // a run saved with a later copy of this world
	}
	const uint32_t t = run_ticks();
	if (t % CHECK_EVERY == 0) {
		check_creatures();
	}
	if (s_run.settings.food != FOOD_NONE && t % RESTOCK_EVERY == 0) {
		restock();
	}
	if (t > 0 && t % TICKS_PER_HOUR == 0) {
		int alive = 0;
		for (auto* obj : *g_engine_context.objects) {
			if (auto* c = obj->as_creature(); c && c->genus == 1 && !c->dead) {
				alive++;
			}
		}
		s_run.population.push_back(alive);
		if (s_run.settings.snapshot_hours > 0 && (t / TICKS_PER_HOUR) % static_cast<uint32_t>(s_run.settings.snapshot_hours) == 0) {
			take_snapshot();
		}
		save();
	}
}

bool hand_locked() {
	return s_run.active && s_run.settings.variant != VARIANT_FERAL;
}

bool kit_allowed(const std::string& kit) {
	if (!hand_locked()) {
		return true;
	}
	// looking is allowed; anything that changes the world isn't
	return kit == "Observe" || kit == "Owner" || kit == "Score" || kit == "Funeral";
}

int generation(const std::string& moniker) {
	auto it = s_run.generations.find(moniker);
	return it != s_run.generations.end() ? it->second : 0;
}

void world_changed(bool new_world) {
	if (new_world) {
		s_run = Run{};
	} else {
		load();
	}
	s_show_sheet = s_show_dashboard = false;
	s_sheet = s_run.settings;
	g_engine_context.pointer->m_locked = hand_locked();
}

void save_for_world(const std::string& file_name) {
	write_run(file_name);
}

void open_start_sheet() {
	s_sheet = s_run.settings;
	s_show_sheet = true;
}

void open_dashboard() {
	s_show_dashboard = true;
}

uint32_t run_started_tick() {
	return s_run.started;
}

void draw_windows() {
	const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
	if (s_show_sheet) {
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (ImGui::Begin("Start a wolfling run", &s_show_sheet, flags)) {
			ImGui::TextWrapped("Hatch some Norns, then leave them to look after themselves and see which genes are passed on.");
			ImGui::SeparatorText("Rules");
			int v = s_sheet.variant;
			ImGui::RadioButton("Strict: no hand, and only the kits that look", &v, VARIANT_STRICT);
			ImGui::RadioButton("Relaxed: set the world up first, then hands off", &v, VARIANT_RELAXED);
			ImGui::RadioButton("Feral: you can still step in", &v, VARIANT_FERAL);
			s_sheet.variant = static_cast<Variant>(v);
			ImGui::SeparatorText("Food");
			int f = s_sheet.food;
			ImGui::RadioButton("None: only what the world has", &f, FOOD_NONE);
			ImGui::RadioButton("Restock: the world's cheese is put back, honey pots refill (recommended)", &f, FOOD_RESTOCK);
			ImGui::RadioButton("Restock, plus a row of floating cheese in every room", &f, FOOD_LINE);
			s_sheet.food = static_cast<Food>(f);
			ImGui::SeparatorText("Keeping the run going");
			ImGui::SetNextItemWidth(120);
			ImGui::SliderInt("Add an egg when fewer Norns are alive than", &s_sheet.population_floor, 0, 12, s_sheet.population_floor ? "%d" : "off");
			ImGui::Checkbox("Name Norns by generation (G1 A..., G2 B...)", &s_sheet.names_by_generation);
			ImGui::SetNextItemWidth(120);
			ImGui::SliderInt("Save a snapshot every (game hours)", &s_sheet.snapshot_hours, 0, 48, s_sheet.snapshot_hours ? "%d" : "off");
			ImGui::Separator();
			if (ImGui::Button(s_run.active ? "Start a new run" : "Start the run")) {
				start(s_sheet);
				s_show_sheet = false;
			}
			if (s_run.active) {
				ImGui::SameLine();
				if (ImGui::Button("End the current run")) {
					stop();
					s_show_sheet = false;
				}
			}
		}
		ImGui::End();
	}

	if (s_show_dashboard) {
		ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
		if (ImGui::Begin("Wolfling run", &s_show_dashboard, ImGuiWindowFlags_NoCollapse)) {
			const double run_hours = hours(run_ticks());
			ImGui::Text("%s run %d: %s, %.1f game hours", s_run.active ? "Current" : "Last", s_run.number, variant_name(s_run.settings.variant), run_hours);
			ImGui::Text("Births %d   Deaths %d   Eggs added %d", s_run.births, s_run.deaths, s_run.eggs_added);
			if (s_run.deaths > 0) {
				ImGui::Text("Average lifespan %.1f game hours", static_cast<double>(s_run.total_lifespan_hours) / s_run.deaths);
				for (auto& [cause, n] : s_run.death_causes) {
					ImGui::BulletText("%d died of %s", n, cause.c_str());
				}
			}
			if (!s_run.population.empty()) {
				std::vector<float> values(s_run.population.begin(), s_run.population.end());
				const float top = *std::max_element(values.begin(), values.end());
				ImGui::PlotLines("##population", values.data(), static_cast<int>(values.size()), 0, "Norns alive, each game hour", 0, std::max(1.f, top) * 1.1f, ImVec2(-1, 80));
			}
			ImGui::SeparatorText("Norns");
			if (ImGui::BeginTable("##norns", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("Generation");
				ImGui::TableSetupColumn("Age (hours)");
				ImGui::TableSetupColumn("");
				ImGui::TableHeadersRow();
				for (auto* obj : *g_engine_context.objects) {
					auto* c = obj->as_creature();
					if (!c || c->genus != 1) {
						continue;
					}
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(label(*c).c_str());
					ImGui::TableNextColumn();
					ImGui::Text("G%d", generation(c->moniker));
					ImGui::TableNextColumn();
					ImGui::Text("%.1f", hours(c->age_ticks));
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(c->dead ? "dead" : c->sex == 2 ? "female" : "male");
				}
				ImGui::EndTable();
			}
			// the best Norns, saved as breeding stock for the next run
			if (ImGui::Button("Export champions")) {
				std::vector<Creature*> living;
				for (auto* obj : *g_engine_context.objects) {
					if (auto* c = obj->as_creature(); c && c->genus == 1 && !c->dead) {
						living.push_back(c);
					}
				}
				std::sort(living.begin(), living.end(), [](Creature* a, Creature* b) { return a->age_ticks > b->age_ticks; });
				const fs::path folder = current_world_folder() / "Wolfling Champions" / fmt::format("Run {}", s_run.number);
				std::error_code err;
				fs::create_directories(folder, err);
				int exported = 0;
				for (size_t i = 0; i < living.size() && i < 4; ++i) {
					try {
						export_creature(*living[i], false, (folder / (export_file_name(*living[i]) + ".exp")).string());
						exported++;
					} catch (std::exception& e) {
						fmt::print("WARN [Wolfling] couldn't export {}: {}\n", living[i]->moniker, e.what());
					}
				}
				note(fmt::format("{} champions were saved in {} (they stay in the world)", exported, folder.string()));
			}
			ImGui::SetItemTooltip("Saves copies of the four oldest living Norns as .exp files in \"Wolfling Champions\"");
			ImGui::SeparatorText("What happened");
			if (ImGui::BeginChild("##events", ImVec2(0, 160), ImGuiChildFlags_Borders)) {
				for (auto it = s_run.events.rbegin(); it != s_run.events.rend(); ++it) {
					ImGui::TextWrapped("%.1f h  %s", hours(it->tick >= s_run.started ? it->tick - s_run.started : 0), it->text.c_str());
				}
			}
			ImGui::EndChild();
			if (ImGui::Button(s_run.active ? "Change or end the run..." : "Start a run...")) {
				open_start_sheet();
			}
		}
		ImGui::End();
	}
}

} // namespace Wolfling
