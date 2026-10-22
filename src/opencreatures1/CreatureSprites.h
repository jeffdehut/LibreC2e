#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace sfc {
struct CreatureV1;
}

// Creatures 1 builds each creature's sprite file (Images/<moniker>.spr) by joining 12
// body-part sprite files end to end, in the order head, body, left thigh, left shin,
// left foot, right thigh, right shin, right foot, left humerus, left radius, right
// humerus, right radius.
//
// Each body-part sprite is named <part letter><sex digit><age digit><breed digit>, e.g.
// "C412" is the left thigh of a female child of breed 2. The breed comes from the
// creature's appearance gene for that body region (head, body, legs or arms).

enum class CreatureBodyRegion {
	HEAD = 0,
	BODY = 1,
	LEGS = 2,
	ARMS = 3,
};

// The name of a body-part sprite (without extension), or an empty string if no
// suitable sprite exists. When the requested age doesn't exist for this breed, falls
// back to the nearest younger age, e.g. the Santa breed only has baby-sized sprites.
std::string creature_body_part_sprite(char part, int sex_digit, int age, int breed);

// The 12 body-part sprite names making up this creature's sprite file. Breeds are
// chosen per body region so the joined sprites match the image sizes recorded in the
// creature's saved gallery.
std::vector<std::string> creature_body_part_sprites_for_saved_gallery(const sfc::CreatureV1& creature);
