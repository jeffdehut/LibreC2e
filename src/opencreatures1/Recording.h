#pragma once

#include "ImageGallery.h"
#include "common/Image.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Object;

// World recordings (not in the original): a world recorded as it runs, its picture and its
// creatures, to be played back later, forwards or backwards, without anything in it being
// changed. A recording (a .c1rec file) holds everything it needs to be watched with the game:
// the game's own sprites by name (checked against GOG's list of the installation's files, so
// each Norn is rebuilt from its body parts and colours), and the pictures of any others (from
// COBs, or changed) in full; what was drawn each tick; every creature's state every 16 ticks
// (chemicals, brain, what it was doing and looking at); what happened (births, deaths, eggs,
// speech); and the whole world now and then, as a saved world. The world's time is the tick,
// so its day and night play back too.
//
// The file (all numbers little-endian; "var" is a LEB128 number, "svar" a zigzag one):
//   "C1REC\r\n\x1a", u32 version (2; 1 kept every sprite's pictures), u32 0
//   chunks: u32 type, u32 stored size, u32 size inflated, u32 first tick, then the chunk's
//     bytes deflated (zlib). Types:
//     INFO  var n, n x (string key, string value); strings are var length + UTF-8
//     MAP   the rooms (var n, n x 5 svar) and the 261 ground levels (var)
//     ASET  a sprite file as drawn: var id, string name, u8 how (from version 2), then
//           how 0, its pictures: svar base, u8 background, var offsets (svar each), var
//           images, each var width, var height, u8 format, its pixels, its see-through
//           colour (r g b a), and for 8-bit images the palette (256 x r g b a);
//           how 1, the game's own files: svar base, var count (0: all of each file, joined),
//           u8 background, u8 remapped, the remap (256 bytes) if so, var files, each
//           (string file, string its MD5 in hex)
//     VIEW  a run of ticks' pictures, the first in full and the rest as changes (below)
//     CREA  a run of creature samples, each creature's after the first as XOR with its last
//           (version 2 adds what it's paying attention to and where it's heading)
//     EVNT  var n, n x (var tick, u8 kind, u32 creature, string text)
//     SNAP  the whole world, as a saved world (.sfc)
//     INDX  every chunk: var n, n x (u32 type, var tick, var last tick, var offset)
//   at the end, "C1RECEND" and u64 the INDX chunk's offset. A recording cut short (the game
//   stopped) has no index; it's read chunk by chunk instead.
//   A tick's picture: var tick (after the run's first), var changed, each u32 owner, var part,
//   u8 what changed (1 sprite, 2 frame, 4 x, 8 y, 16 plane, 32 new), then those (var sprite,
//   var frame, svar x and y in sixteenths of a pixel and svar plane, the last three as
//   differences); var removed, each u32 owner, var part; var owners, each u32 owner, u8 kind
//   (Owner::Kind), u8 family, genus, species.
namespace Recording {

constexpr const char* EXTENSION = ".c1rec";

struct Owner {
	enum Kind : uint8_t { OBJECT, CREATURE, HAND, EGG };
	uint32_t uid = 0;
	uint8_t kind = OBJECT, family = 0, genus = 0, species = 0;
};

// something drawn: a frame of a sprite, by its owner and the owner's part number
struct Item {
	uint32_t owner = 0;
	uint32_t part = 0;
	uint32_t asset = 0;
	int32_t frame = 0;
	float x = 0, y = 0;
	int32_t z = 0;
};

// a sprite file as the world drew it: its pictures, or (from_game) the game's own files to
// load them from, with their MD5s
struct Asset {
	uint32_t id = 0;
	std::string name;
	int32_t absolute_base = 0;
	bool background = false;
	std::vector<int32_t> offsets;
	MultiImage images;
	bool from_game = false;
	GalleryOrigin origin;
	std::vector<std::string> md5;
};

// a creature at a moment
struct Sample {
	enum Flags : uint8_t { ASLEEP = 1, DEAD = 2, HELD = 4 };
	uint32_t uid = 0;
	std::string moniker, name, doing_to; // doing_to: what it's paying attention to
	uint8_t sex = 0, genus = 0, life_stage = 0, flags = 0, expression = 0, health = 0, gestation = 0;
	float x = 0, y = 0, width = 0, height = 0; // its body
	uint32_t age_ticks = 0;
	int32_t decision = -1;
	std::array<uint8_t, 256> chemicals{};
	std::vector<uint16_t> lobe_sizes; // the brain's lobes, in order
	std::vector<uint8_t> neurons; // every neuron's output, lobe after lobe
	// what it's paying attention to (an owner, 0 for nothing) and where it's heading
	uint32_t attention = 0;
	bool has_target = false;
	float target_x = 0, target_y = 0;
};

struct Event {
	enum Kind : uint8_t { STARTED, APPEARED, DIED, EGG, PREGNANT, SAID };
	uint32_t tick = 0;
	uint8_t kind = STARTED;
	uint32_t creature = 0;
	std::string text;
};

// --- recording the world as it runs ---

bool recording();
// starts recording to a file (stopping any recording first); throws if it can't be written
void start(const std::filesystem::path& file);
void stop();
const std::filesystem::path& file();
// ticks recorded so far, and the file's size
uint32_t ticks_recorded();
uint64_t bytes_written();
// Each frame the world is drawn: begin_capture() says whether this one is recorded (a tick
// not yet recorded); if so, set_owner() before each object draws, which notes each frame it
// draws, then end_capture().
bool begin_capture();
extern bool g_capturing;
void set_owner(const Object&);
void note(const ImageGallery& gallery, int32_t frame, float x, float y, int32_t z);
void end_capture();

// --- reading a recording ---

class Player {
  public:
	// reads the recording's index, sprites and events; throws if it isn't one
	explicit Player(const std::filesystem::path& file);

	const std::filesystem::path& file() const { return m_file; }
	const std::map<std::string, std::string>& info() const { return m_info; }
	uint32_t first_tick() const { return m_first; }
	uint32_t last_tick() const { return m_last; }
	const std::vector<Event>& events() const { return m_events; }
	const std::vector<Asset>& assets() const { return m_assets; }
	const Asset* background() const;

	// what was drawn at a tick (the last recorded tick at or before it), in owner order
	const std::vector<Item>& items_at(uint32_t tick);
	// what to draw at a moment between recorded ticks: whatever moved a little from one to the
	// next is drawn part of the way along, so playback is smooth at any speed
	void items_between(double position, std::vector<Item>& out);
	// the tick items_at() shows for a tick, and the recorded ticks either side
	uint32_t shown_tick(uint32_t tick);
	uint32_t next_tick(uint32_t tick);
	uint32_t previous_tick(uint32_t tick);
	const Owner* owner(uint32_t uid) const;
	// the creatures at a tick: the last sample at or before it
	const std::vector<Sample>& creatures_at(uint32_t tick);
	// the saved world nearest before a tick, and its tick, or empty
	std::vector<uint8_t> world_at(uint32_t tick, uint32_t* at = nullptr);

  private:
	struct Chunk {
		uint32_t type, tick, last;
		uint64_t offset;
	};
	std::vector<uint8_t> inflate(const Chunk&) const;
	const Chunk* run_for(const std::vector<Chunk>& runs, uint32_t tick) const;
	using Frames = std::vector<std::pair<uint32_t, std::vector<Item>>>;
	const Frames& decode_view(const Chunk&);
	void decode_creatures(const Chunk&);

	std::filesystem::path m_file;
	std::vector<uint8_t> m_bytes;
	std::map<std::string, std::string> m_info;
	std::vector<Asset> m_assets;
	std::vector<Event> m_events;
	std::vector<Chunk> m_views, m_samples, m_worlds;
	std::map<uint32_t, Owner> m_owners;
	uint32_t m_first = 0, m_last = 0, m_version = 0;
	// the two runs decoded last (a picture per recorded tick), and the creature run
	struct Decoded {
		uint64_t offset = ~0ull;
		uint64_t used = 0;
		Frames frames;
	};
	std::array<Decoded, 2> m_decoded;
	uint64_t m_uses = 0;
	uint64_t m_sample_offset = ~0ull;
	std::vector<std::pair<uint32_t, std::vector<Sample>>> m_sampled;
	std::vector<Item> m_none;
	std::vector<Sample> m_nobody;
};

// the MD5 of some bytes, in hex
std::string md5_hex(const uint8_t* data, size_t size);

// the stages of a creature's life, by its age (0 Baby to 6 Senile)
const char* life_stage_name(uint8_t stage);

} // namespace Recording
