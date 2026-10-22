#pragma once

#include <stdint.h>
#include <string>
#include <vector>

struct Creature;

// Photographs (Clean Specs/BATCH4-ANSWERS.md, Q14a). The Owner's Kit's camera jumps the view
// to the selected creature (its ground at 5/8 of the view's height) and takes a 120 x 140
// picture of whatever is drawn around it. Each creature has an album; the Funeral Kit shows
// a dead creature's album.
//
// The original keeps albums in "<moniker>.Photo Album", but its layout inside isn't known, so
// this engine keeps them in its own file, "<moniker>.openc1.photos" in the main directory:
// "OC1P", a 16-bit count, then for each photo a 32-bit time (seconds since 1970), a 16-bit
// width and height, and the pixels in the game's palette. Photos don't travel with exported
// creatures, as in the original.
namespace PhotoAlbum {
void world_changed();

struct Photo {
	uint32_t time = 0;
	int32_t width = 0;
	int32_t height = 0;
	std::vector<uint8_t> pixels;
};

constexpr int32_t PHOTO_WIDTH = 120;
constexpr int32_t PHOTO_HEIGHT = 140;

std::vector<Photo> load(const std::string& moniker);
void save(const std::string& moniker, const std::vector<Photo>& photos);

// Takes a photo of the creature: returns false if it can't be framed in the view.
bool take_photo(Creature& creature);
// A photo for the album without moving the view (the automatic birthday and life-stage
// photos). Returns false if another photo is still being taken.
bool take_photo_quietly(Creature& creature);

// The time a photo was taken, as the album labels it: "14:05 17 October 2026".
std::string label(const Photo& photo);

// Writes a photo as an 8-bit .bmp file in the main directory; returns false on failure.
bool save_bmp(const Photo& photo, const std::string& file_name);

// Draws a creature's album on the album page picture, with Prev/Next; with controls, also
// the camera, Delete and Save As (the Owner's Kit). Returns the photo shown, or -1.
int32_t draw(Creature* creature, const std::string& moniker, bool controls);

} // namespace PhotoAlbum
