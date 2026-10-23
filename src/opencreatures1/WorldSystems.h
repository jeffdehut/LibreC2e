#pragma once

#include <filesystem>
#include <stdint.h>
#include <string>
#include <vector>

struct Creature;

// World-wide systems, following Clean Specs/BATCH3-ANSWERS.md: the score counters (Q30),
// the selected creature's health (Q1) and autosaving (Q10).

// Runs once per tick, after the objects.
void world_systems_tick();

// Health as the status bar shows it: Glycogen (chemical 59) as a percentage, 0 if dead.
int32_t creature_health_percent(const Creature& creature);
// C1 status stage: zero when nonpregnant, otherwise ceiling(progesterone / 32).
int32_t creature_gestation_stage(const Creature&);

// The score as the status bar shows it: 256 x natural eggs + creature-time, at most 99999
// (batch 4, Q26).
uint32_t player_score();

// The number of creatures that aren't dead.
int32_t count_living_creatures();

// The world file being played, relative to the main directory (e.g. "World.openc1.sfc").
// Saving and autosaving write it.
void set_world_file_name(const std::string& name);
const std::string& get_world_file_name();

// Each world keeps its files together (not in the original, which keeps everything in its own
// folder): Worlds/<name>/ in the game folder holds <name>.sfc and everything that belongs to it
// (graveyard, family, wolfling record, add-on allowances, registrations, Photos/, Norns/,
// Eggs/, Wolfling Snapshots/), and its autosaves are in Worlds/<name>/Autosaves/. The original's
// own worlds (Eden.sfc, World.sfc) stay in the game folder, never written to.
//   world_base_name("Brave.autosave.2.sfc") is "Brave"
std::string world_base_name(const std::string& world_file);
// the folder a world file and its companions belong in (Autosaves/ for an autosave)
std::filesystem::path world_folder(const std::string& world_file);
// the playing world's own folder (Worlds/<name>/, also for its autosaves)
std::filesystem::path current_world_folder();
// where a world file is: its folder, or the game folder (the originals, and any not moved
// yet); empty if it's nowhere
std::filesystem::path find_world(const std::string& world_file);
// every world file there is, by file name
std::vector<std::string> list_world_files();
// Moves loose worlds and their companions into their folders, copying each file to
// Backup/Before reorganising/ first. Run at start-up, before a world is loaded. Returns how
// many files moved.
int organise_world_files();

// Saves the world to a file in its folder.
void save_world(const std::string& file_name);
// Validated local save name; original templates and directory traversal rejected.
std::string world_save_name(std::string name);
std::string last_saved_world();

// Every 6000 ticks: saves the world, then copies it and every living creature's sprite file
// into TempBu, then Backup (Q10). Does nothing when autosaving is switched off.
void set_autosave_enabled(bool enabled);
bool autosave_enabled();
std::string take_autosave_error();
