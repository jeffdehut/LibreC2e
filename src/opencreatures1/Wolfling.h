#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Wolfling runs: hatching Norns and leaving them to look after themselves, to see which genes
// survive (see WOLFLING-RUNS.md and PLAYER-RESEARCH.md). Not in the original; the engine helps
// with what runners did by hand or with add-ons: keeping food stocked, keeping the population
// going, naming by generation, saving snapshots, and recording what happened.
namespace Wolfling {

// The family registry (the app's, Family.cpp) tells the run a Norn's name by moniker and
// whether a name was ever given in this world, so names are never repeated.
void set_family_lookup(std::function<std::string(const std::string& moniker)> name_of, std::function<bool(const std::string& name)> taken);

// debugging: n names for a generation, as the run would give them (none are recorded)
std::vector<std::string> sample_names(int generation, int n);

enum Variant {
	VARIANT_STRICT, // no hand, no kits that change anything
	VARIANT_RELAXED, // the same, once the run has started (set the world up first)
	VARIANT_FERAL, // the player can still step in
};

enum Food {
	FOOD_NONE, // only what the world has (the honest strict run)
	FOOD_RESTOCK, // the world's own cheese put back where it was, honey pots refilled
	FOOD_LINE, // the same, plus a row of floating cheese along the top of each room
};

struct Settings {
	Variant variant = VARIANT_RELAXED;
	Food food = FOOD_RESTOCK;
	int population_floor = 2; // add a Hatchery egg when fewer Norns than this are alive (0: off)
	bool names_by_generation = true; // G1 names start with A, G2 with B, ...
	int snapshot_hours = 6; // save a numbered copy of the world every n game hours (0: off)
};

bool active();
// keeps about two food items per living Norn in the world (not in the original); call once a
// game minute
void keep_food_for_population();
const Settings& settings();
void start(const Settings&);
void stop();
// true once after a run has ended by itself, every creature (Grendels too) having died
bool take_died_out();

// once a game tick, while the game runs
void tick();
// the toolbar badge (modern) and the run's windows
// the world tick the run started at
uint32_t run_started_tick();
void draw_windows();
void open_start_sheet();
void open_dashboard();

// what a run allows: the hand, and each kit by its toolbar name
bool hand_locked();
bool kit_allowed(const std::string& kit);

// a creature's generation (1 for Hatchery eggs), 0 if unknown
int generation(const std::string& moniker);

// Load this world's run after its final save name is set. A fresh world starts without a run.
void world_changed(bool new_world = false);
// Save alongside this world file, including Save As copies.
void save_for_world(const std::string& file_name);

} // namespace Wolfling
