#pragma once

#include "objects/ObjectHandle.h"
#include "Graveyard.h"

#include <array>
#include <stdint.h>
#include <string>
#include <vector>

// The status bar shows up to 4 recent events; up to 16 dead creatures wait on death row for
// the Funeral Kit (batch 3, Q31).
constexpr size_t MAX_EVENTS = 4;
constexpr size_t MAX_DEATH_ROW = 16;

// World-wide state stored in Creatures 1 world files. Much of it isn't used by the engine
// yet, but it's loaded and saved so that saving a world doesn't lose it.
struct WorldState {
	struct FavoritePlace {
		std::string name;
		int16_t x = 0;
		int16_t y = 0;
	};

	struct Bacterium {
		uint8_t state = 0;
		uint8_t antigen = 0;
		uint8_t fatal_level = 0;
		uint8_t infect_level = 0;
		std::array<uint8_t, 4> toxins{};
	};

	uint32_t time_of_day = 0;
	std::array<Bacterium, 100> bacteria;

	ObjectHandle current_norn;
	std::array<FavoritePlace, 6> favorite_places;
	std::vector<std::string> speech_history;
	std::vector<ObjectHandle> death_row;
	// Value snapshots survive scripts deleting the body before the next UI refresh.
	std::vector<GraveRecord> pending_graves;
	std::vector<ObjectHandle> events;
	std::vector<std::string> stuffed_norns; // monikers

	// The five score counters (batch 3, Q30). The first two fields aren't used: the original
	// leaves them uninitialised (0xCDCDCDCD) in every saved world.
	uint32_t current_score = 0;
	uint32_t current_health = 0;
	uint32_t hatchery_eggs = 0;
	uint32_t natural_eggs = 0;
	uint32_t dead_norns = 0;
	uint32_t live_norns = 0;
	uint32_t breeders_score = 0; // creature-time: living creatures added every 1200 ticks
	uint32_t tick = 0;
};

// C1 disease mutation, retaining chemical identities within the disease mapping.
WorldState::Bacterium mutate_bacterium(WorldState::Bacterium);
