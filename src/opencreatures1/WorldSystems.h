#pragma once

#include <filesystem>
#include <stdint.h>
#include <string>

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

// Saves the world to a file in the main directory.
void save_world(const std::string& file_name);
// Validated local save name; original templates and directory traversal rejected.
std::string world_save_name(std::string name);
std::string last_saved_world();

// Every 6000 ticks: saves the world, then copies it and every living creature's sprite file
// into TempBu, then Backup (Q10). Does nothing when autosaving is switched off.
void set_autosave_enabled(bool enabled);
bool autosave_enabled();
std::string take_autosave_error();
