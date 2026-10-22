#pragma once

#include <stdint.h>
#include <string>

// Not in the original: a family registry of every Norn this game folder has seen (parents
// from each creature's own record, batch 6 Q9, kept after it dies or leaves), the automatic
// album photos (at birth and at each new life stage), and a family tree window.
namespace Family {

// once a game tick
void tick();
// the family tree window
void open_tree();
void draw_windows();

struct Member {
	std::string moniker;
	std::string name;
	std::string mother;
	std::string father;
	int sex = 0; // 1 male, 2 female
	bool dead = false;
	bool here = false; // in the world now
	// its face, kept for when it isn't here: the head's sprite file and frame
	std::string face_sprite;
	int32_t face_base = 0, face_count = 0, face_frame = -1;
};
// a creature's record, or nullptr
const Member* find(const std::string& moniker);
// its generation: 1 for a Norn with no known parents (from the Hatchery, or imported), else one
// more than its later parent's; 0 if it isn't in the registry
int generation_of(const std::string& moniker);
// whether any Norn this world has seen has (or had) that name
bool name_taken(const std::string& name);

// forget the world (call after loading another one; the registry is per game folder)
void world_changed();

} // namespace Family
