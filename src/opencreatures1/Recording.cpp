#include "Recording.h"

#include "EngineContext.h"
#include "ImageGallery.h"
#include "MapManager.h"
#include "PointerManager.h"
#include "SFCSerialization.h"
#include "ViewportManager.h"
#include "WorldState.h"
#include "WorldSystems.h"
#include "common/Exception.h"
#include "common/io/VectorWriter.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "fileformats/sfc/CBrain.h"
#include "fileformats/sfc/SFCFile.h"
#include "objects/Bubble.h"
#include "objects/Creature.h"
#include "objects/Object.h"
#include "objects/ObjectManager.h"
#include "objects/ObjectNames.h"
#include "ImageManager.h"
#include "../../third_party/librecreatures/C1Palette.h"
#include "PathManager.h"
#include "common/io/FileReader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fmt/core.h>
#include <fstream>
#include <set>
#include <unordered_map>
#include <zlib.h>

namespace fs = std::filesystem;

namespace Recording {

namespace {

constexpr char MAGIC[8] = {'C', '1', 'R', 'E', 'C', '\r', '\n', '\x1a'};
constexpr char END[8] = {'C', '1', 'R', 'E', 'C', 'E', 'N', 'D'};
constexpr uint32_t VERSION = 2;
// a picture run lasts 100 ticks, a creature run 32 samples, 16 ticks apart; the whole
// world is saved every 3000 ticks (about five minutes)
constexpr uint32_t VIEW_RUN = 100;
constexpr uint32_t SAMPLE_EVERY = 16;
constexpr uint32_t SAMPLES_PER_RUN = 32;
constexpr uint32_t WORLD_EVERY = 3000;

constexpr uint32_t fourcc(const char (&s)[5]) {
	return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8 |
		   static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24;
}
constexpr uint32_t INFO = fourcc("INFO"), MAP = fourcc("MAP "), ASET = fourcc("ASET"), VIEW = fourcc("VIEW"), CREA = fourcc("CREA"),
				   EVNT = fourcc("EVNT"), SNAP = fourcc("SNAP"), INDX = fourcc("INDX");

// --- writing and reading numbers ---

struct Out {
	std::vector<uint8_t> b;
	void u8(uint8_t v) { b.push_back(v); }
	void u32(uint32_t v) {
		for (int i = 0; i < 4; ++i) {
			b.push_back(static_cast<uint8_t>(v >> (8 * i)));
		}
	}
	void u64(uint64_t v) {
		for (int i = 0; i < 8; ++i) {
			b.push_back(static_cast<uint8_t>(v >> (8 * i)));
		}
	}
	void var(uint64_t v) {
		do {
			const auto byte = static_cast<uint8_t>(v & 0x7f);
			v >>= 7;
			b.push_back(v ? static_cast<uint8_t>(byte | 0x80) : byte);
		} while (v);
	}
	void svar(int64_t v) { var((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
	void f32(float v) {
		uint32_t u;
		std::memcpy(&u, &v, 4);
		u32(u);
	}
	void str(const std::string& s) {
		var(s.size());
		b.insert(b.end(), s.begin(), s.end());
	}
	void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
};

struct In {
	const uint8_t* p;
	const uint8_t* end;
	void need(size_t n) const {
		if (static_cast<size_t>(end - p) < n) {
			throw Exception("The recording is damaged (a chunk ends too soon)");
		}
	}
	bool done() const { return p >= end; }
	uint8_t u8() {
		need(1);
		return *p++;
	}
	uint32_t u32() {
		need(4);
		uint32_t v = 0;
		for (int i = 0; i < 4; ++i) {
			v |= static_cast<uint32_t>(*p++) << (8 * i);
		}
		return v;
	}
	uint64_t u64() {
		need(8);
		uint64_t v = 0;
		for (int i = 0; i < 8; ++i) {
			v |= static_cast<uint64_t>(*p++) << (8 * i);
		}
		return v;
	}
	uint64_t var() {
		uint64_t v = 0;
		for (int shift = 0; shift < 64; shift += 7) {
			const uint8_t byte = u8();
			v |= static_cast<uint64_t>(byte & 0x7f) << shift;
			if (!(byte & 0x80)) {
				return v;
			}
		}
		throw Exception("The recording is damaged (a number is too long)");
	}
	uint32_t var32() { return static_cast<uint32_t>(var()); }
	int64_t svar() {
		const uint64_t v = var();
		return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
	}
	float f32() {
		const uint32_t u = u32();
		float v;
		std::memcpy(&v, &u, 4);
		return v;
	}
	std::string str() {
		const auto n = static_cast<size_t>(var());
		need(n);
		std::string s(reinterpret_cast<const char*>(p), n);
		p += n;
		return s;
	}
	const uint8_t* bytes(size_t n) {
		need(n);
		const uint8_t* at = p;
		p += n;
		return at;
	}
};

size_t bytes_per_pixel(imageformat f) {
	switch (f) {
		case if_index8: return 1;
		case if_rgb555:
		case if_rgb565: return 2;
		default: return 3;
	}
}

int32_t sixteenths(float v) {
	return static_cast<int32_t>(std::lround(v * 16));
}

// MD5 (RFC 1321), to check a sprite file is the game's own
struct Md5 {
	uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
	uint64_t length = 0;
	uint8_t buffer[64] = {};
	size_t used = 0;

	void block(const uint8_t* p) {
		static const auto K = [] {
			std::array<uint32_t, 64> k{};
			for (size_t i = 0; i < 64; ++i) {
				k[i] = static_cast<uint32_t>(std::floor(std::fabs(std::sin(static_cast<double>(i + 1))) * 4294967296.0));
			}
			return k;
		}();
		static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
			4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
		uint32_t w[16];
		for (size_t i = 0; i < 16; ++i) {
			w[i] = static_cast<uint32_t>(p[i * 4]) | static_cast<uint32_t>(p[i * 4 + 1]) << 8 | static_cast<uint32_t>(p[i * 4 + 2]) << 16 | static_cast<uint32_t>(p[i * 4 + 3]) << 24;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
		for (size_t i = 0; i < 64; ++i) {
			uint32_t f;
			size_t g;
			if (i < 16) {
				f = (b & c) | (~b & d), g = i;
			} else if (i < 32) {
				f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
			} else if (i < 48) {
				f = b ^ c ^ d, g = (3 * i + 5) % 16;
			} else {
				f = c ^ (b | ~d), g = (7 * i) % 16;
			}
			const uint32_t x = a + f + K[i] + w[g];
			a = d;
			d = c;
			c = b;
			b = b + ((x << R[i]) | (x >> (32 - R[i])));
		}
		h[0] += a, h[1] += b, h[2] += c, h[3] += d;
	}
	void add(const uint8_t* p, size_t n) {
		length += n;
		while (n > 0) {
			const size_t take = std::min(n, 64 - used);
			std::memcpy(buffer + used, p, take);
			used += take, p += take, n -= take;
			if (used == 64) {
				block(buffer);
				used = 0;
			}
		}
	}
	std::string hex() {
		const uint64_t bits = length * 8;
		const uint8_t one = 0x80, zero = 0;
		add(&one, 1);
		while (used != 56) {
			add(&zero, 1);
		}
		uint8_t size[8];
		for (int i = 0; i < 8; ++i) {
			size[i] = static_cast<uint8_t>(bits >> (8 * i));
		}
		add(size, 8);
		std::string out;
		for (uint32_t v : h) {
			for (int i = 0; i < 4; ++i) {
				out += fmt::format("{:02x}", (v >> (8 * i)) & 0xff);
			}
		}
		return out;
	}
};

std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// GOG's list of the installation's files and their MD5s (goggame-*.hashdb in the game
// folder: a zip holding a 12-byte header, then 1056-byte records, the path, Windows style, in
// the first 1024 and the MD5 in hex in the last 32), by lower-case path with forward slashes.
// Empty if there isn't one.
const std::map<std::string, std::string>& game_files() {
	static std::map<std::string, std::string> files;
	static uint64_t loaded_for = ~0ull;
	if (loaded_for == g_engine_context.generation()) {
		return files;
	}
	loaded_for = g_engine_context.generation();
	files.clear();
	std::error_code err;
	const fs::path folder = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "");
	for (const auto& entry : fs::directory_iterator(folder, err)) {
		const auto name = lower(entry.path().filename().string());
		if (name.rfind("goggame-", 0) != 0 || entry.path().extension() != ".hashdb") {
			continue;
		}
		try {
			std::ifstream in(entry.path(), std::ios::binary);
			std::vector<uint8_t> zip((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			// the zip's first file, through its central directory
			size_t end = zip.size() >= 22 ? zip.size() - 22 : 0;
			while (end > 0 && !(zip[end] == 'P' && zip[end + 1] == 'K' && zip[end + 2] == 5 && zip[end + 3] == 6)) {
				end--;
			}
			In e{zip.data() + end + 16, zip.data() + zip.size()};
			const uint32_t central = e.u32();
			In c{zip.data() + central, zip.data() + zip.size()};
			if (c.u32() != 0x02014b50) {
				continue;
			}
			c.bytes(6);
			const uint32_t method = c.u8() | static_cast<uint32_t>(c.u8()) << 8;
			c.bytes(8);
			const uint32_t packed = c.u32(), size = c.u32();
			c.bytes(14); // the name's, extra field's and comment's lengths, the disk, attributes
			const uint32_t local = c.u32();
			In l{zip.data() + local + 26, zip.data() + zip.size()};
			const uint32_t name_length = l.u8() | static_cast<uint32_t>(l.u8()) << 8, extra = l.u8() | static_cast<uint32_t>(l.u8()) << 8;
			const uint8_t* data = l.bytes(name_length + extra + packed) + name_length + extra;
			std::vector<uint8_t> db(size);
			if (method == 0) {
				std::memcpy(db.data(), data, std::min<size_t>(size, packed));
			} else {
				z_stream z{};
				inflateInit2(&z, -MAX_WBITS);
				z.next_in = const_cast<Bytef*>(data);
				z.avail_in = packed;
				z.next_out = db.data();
				z.avail_out = size;
				const int result = inflate(&z, Z_FINISH);
				inflateEnd(&z);
				if (result != Z_STREAM_END) {
					continue;
				}
			}
			constexpr size_t RECORD = 1056;
			for (size_t at = 12; at + RECORD <= db.size(); at += RECORD) {
				const char* path = reinterpret_cast<const char*>(db.data() + at);
				std::string p(path, strnlen(path, 1024));
				std::replace(p.begin(), p.end(), '\\', '/');
				files[lower(p)] = std::string(reinterpret_cast<const char*>(db.data() + at + 1024), 32);
			}
		} catch (const std::exception& ex) {
			fmt::print("WARN [Recording] couldn't read {}: {}\n", entry.path().string(), ex.what());
		}
	}
	fmt::print("* Recording: {} of the game's files listed by GOG (in {})\n", files.size(), folder.string());
	return files;
}

std::string md5_of_file(const fs::path& path) {
	static std::map<std::string, std::string> known;
	auto it = known.find(path.string());
	if (it != known.end()) {
		return it->second;
	}
	std::ifstream in(path, std::ios::binary);
	Md5 md5;
	char chunk[65536];
	while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0) {
		md5.add(reinterpret_cast<const uint8_t*>(chunk), static_cast<size_t>(in.gcount()));
	}
	return known[path.string()] = md5.hex();
}

// the MD5s of a gallery's sprite files if they're all the game's own and unchanged
bool game_own(const GalleryOrigin& origin, std::vector<std::string>& md5s) {
	const auto& files = game_files();
	if (files.empty() || origin.files.empty()) {
		return false;
	}
	md5s.clear();
	for (const auto& name : origin.files) {
		const auto path = g_engine_context.paths->find_path(PATH_TYPE_IMAGE, name + ".spr");
		if (path.empty()) {
			return false;
		}
		auto it = files.find("images/" + lower(path.filename().string()));
		const std::string md5 = md5_of_file(path);
		if (it == files.end() || it->second != md5) {
			return false;
		}
		md5s.push_back(md5);
	}
	return true;
}

// a sample's bytes (everything but the uid), so a creature's next can be stored as XOR
void write_sample(Out& o, const Sample& s) {
	o.str(s.moniker);
	o.str(s.name);
	o.str(s.doing_to);
	for (uint8_t v : {s.sex, s.genus, s.life_stage, s.flags, s.expression, s.health, s.gestation}) {
		o.u8(v);
	}
	o.f32(s.x);
	o.f32(s.y);
	o.f32(s.width);
	o.f32(s.height);
	o.u32(s.age_ticks);
	o.u32(static_cast<uint32_t>(s.decision));
	o.bytes(s.chemicals.data(), s.chemicals.size());
	o.var(s.lobe_sizes.size());
	for (auto n : s.lobe_sizes) {
		o.var(n);
	}
	o.var(s.neurons.size());
	o.bytes(s.neurons.data(), s.neurons.size());
	o.u32(s.attention);
	o.u8(s.has_target ? 1 : 0);
	o.f32(s.target_x);
	o.f32(s.target_y);
}

Sample read_sample(In& in, uint32_t version) {
	Sample s;
	s.moniker = in.str();
	s.name = in.str();
	s.doing_to = in.str();
	for (uint8_t* v : {&s.sex, &s.genus, &s.life_stage, &s.flags, &s.expression, &s.health, &s.gestation}) {
		*v = in.u8();
	}
	s.x = in.f32();
	s.y = in.f32();
	s.width = in.f32();
	s.height = in.f32();
	s.age_ticks = in.u32();
	s.decision = static_cast<int32_t>(in.u32());
	std::memcpy(s.chemicals.data(), in.bytes(256), 256);
	s.lobe_sizes.resize(static_cast<size_t>(in.var()));
	for (auto& n : s.lobe_sizes) {
		n = static_cast<uint16_t>(in.var());
	}
	const auto count = static_cast<size_t>(in.var());
	const uint8_t* neurons = in.bytes(count);
	s.neurons.assign(neurons, neurons + count);
	if (version >= 2) {
		s.attention = in.u32();
		s.has_target = in.u8() != 0;
		s.target_x = in.f32();
		s.target_y = in.f32();
	}
	return s;
}

// --- the recorder ---

uint32_t uid_of(const Object& o) {
	return static_cast<uint32_t>(o.uid.index) | static_cast<uint32_t>(o.uid.counter) << 16;
}

struct Key {
	uint32_t owner, part;
	bool operator<(const Key& o) const { return owner != o.owner ? owner < o.owner : part < o.part; }
	bool operator==(const Key& o) const { return owner == o.owner && part == o.part; }
};

struct Recorder {
	std::ofstream out;
	fs::path path;
	uint64_t offset = 0;
	struct Indexed {
		uint32_t type, tick, last;
		uint64_t offset;
	};
	std::vector<Indexed> index;
	std::unordered_map<const void*, uint32_t> assets; // a gallery's pictures, by their memory
	std::vector<std::shared_ptr<const MultiImage>> kept; // so their memory isn't reused
	uint32_t next_asset = 0;
	uint64_t generation = 0;

	// this tick's picture, and the last one written
	std::map<Key, Item> now, last;
	std::map<uint32_t, Owner> owners_now;
	std::set<uint32_t> owners_in_run;
	uint32_t owner = 0, part = 0;
	const Creature* creature = nullptr; // the owner, if it's a creature
	Out view;
	uint32_t view_tick = 0, view_last = 0, view_frames = 0;

	Out samples;
	uint32_t sample_tick = 0, sample_last = 0, sample_count = 0, last_sample = 0;
	bool sampled = false;
	std::map<uint32_t, std::vector<uint8_t>> previous_sample;

	std::vector<Event> events;
	uint32_t events_tick = 0;
	std::set<uint32_t> seen_creatures, dead_creatures, pregnant, seen_eggs;
	std::map<uint32_t, std::string> bubbles;

	uint32_t first_tick = 0, last_tick = 0, last_world = 0;
	bool started = false;
};

std::unique_ptr<Recorder> s_rec;
const fs::path s_no_file;

void write_chunk(Recorder& r, uint32_t type, uint32_t tick, uint32_t last, const std::vector<uint8_t>& raw) {
	uLongf size = compressBound(static_cast<uLong>(raw.size()));
	std::vector<uint8_t> packed(size);
	if (compress2(packed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
		throw Exception("Couldn't compress a recording chunk");
	}
	Out head;
	head.u32(type);
	head.u32(static_cast<uint32_t>(size));
	head.u32(static_cast<uint32_t>(raw.size()));
	head.u32(tick);
	r.index.push_back({type, tick, last, r.offset});
	r.out.write(reinterpret_cast<const char*>(head.b.data()), static_cast<std::streamsize>(head.b.size()));
	r.out.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(size));
	r.out.flush();
	r.offset += head.b.size() + size;
	if (!r.out) {
		throw Exception(fmt::format("Couldn't write the recording {}", r.path.string()));
	}
}

uint32_t asset_for(Recorder& r, const ImageGallery& gallery, uint32_t tick) {
	const void* key = gallery.source.get();
	if (!key) {
		return ~0u;
	}
	if (auto it = r.assets.find(key); it != r.assets.end()) {
		return it->second;
	}
	const uint32_t id = r.next_asset++;
	r.assets[key] = id;
	r.kept.push_back(gallery.source);
	Out o;
	o.var(id);
	o.str(gallery.name);
	std::vector<std::string> md5s;
	// a creature's sprites as made from the game's body parts in its colours, if that's
	// exactly what was drawn (its own sprite file, from a saved world, may have been made by
	// the original)
	GalleryOrigin rebuilt;
	bool from_parts = false;
	if (r.creature && !r.creature->sprite_parts.empty() && !(gallery.origin && game_own(*gallery.origin, md5s))) {
		const Creature& c = *r.creature;
		rebuilt.files = c.sprite_parts;
		rebuilt.remapped = true;
		rebuilt.remap = openc1::librecreatures::build_creature_palette_remap(g_engine_context.images->default_palette(), c.pigments[0], c.pigments[1], c.pigments[2], c.pigment_rotation, c.pigment_swap);
		try {
			std::vector<int32_t> offsets;
			const auto images = g_engine_context.images->load_images(rebuilt, offsets);
			from_parts = game_own(rebuilt, md5s) && images.size() == gallery.source->size();
			for (size_t i = 0; from_parts && i < images.size(); ++i) {
				const auto& a = images[i];
				const auto& b = (*gallery.source)[i];
				from_parts = a.width == b.width && a.height == b.height && a.format == b.format && a.data.size() == b.data.size() &&
							 std::memcmp(a.data.data(), b.data.data(), a.data.size()) == 0;
			}
		} catch (const std::exception&) {
			from_parts = false;
		}
	}
	if (from_parts || (gallery.origin && game_own(*gallery.origin, md5s))) {
		const auto& origin = from_parts ? rebuilt : *gallery.origin;
		o.u8(1);
		o.svar(origin.absolute_base);
		o.var(static_cast<uint64_t>(origin.image_count));
		o.u8(origin.background ? 1 : 0);
		o.u8(origin.remapped ? 1 : 0);
		if (origin.remapped) {
			o.bytes(origin.remap.data(), origin.remap.size());
		}
		o.var(origin.files.size());
		for (size_t i = 0; i < origin.files.size(); ++i) {
			o.str(origin.files[i]);
			o.str(md5s[i]);
		}
		write_chunk(r, ASET, tick, tick, o.b);
		return id;
	}
	o.u8(0);
	o.svar(gallery.absolute_base);
	o.u8(gallery.source->size() == 1 && gallery.width(0) >= CREATURES1_WORLD_WIDTH ? 1 : 0);
	o.var(gallery.offsets.size());
	for (auto v : gallery.offsets) {
		o.svar(v);
	}
	o.var(gallery.source->size());
	for (const auto& image : *gallery.source) {
		o.var(static_cast<uint64_t>(image.width));
		o.var(static_cast<uint64_t>(image.height));
		o.u8(static_cast<uint8_t>(image.format));
		const size_t n = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * bytes_per_pixel(image.format);
		if (n && image.data && image.data.size() >= n) {
			o.bytes(image.data.data(), n);
		} else {
			o.b.insert(o.b.end(), n, 0);
		}
		for (uint8_t v : {image.colorkey.r, image.colorkey.g, image.colorkey.b, image.colorkey.a}) {
			o.u8(v);
		}
		if (image.format == if_index8) {
			for (int i = 0; i < 256; ++i) {
				const Color c = image.palette ? image.palette[i] : Color{};
				o.u8(c.r);
				o.u8(c.g);
				o.u8(c.b);
				o.u8(c.a);
			}
		}
	}
	write_chunk(r, ASET, tick, tick, o.b);
	return id;
}

void flush_view(Recorder& r) {
	if (r.view_frames == 0) {
		return;
	}
	Out o;
	o.var(r.view_frames);
	o.bytes(r.view.b.data(), r.view.b.size());
	write_chunk(r, VIEW, r.view_tick, r.view_last, o.b);
	r.view.b.clear();
	r.view_frames = 0;
	// the next run starts with the whole picture
	r.last.clear();
	r.owners_in_run.clear();
}

void flush_samples(Recorder& r) {
	if (r.sample_count == 0) {
		return;
	}
	Out o;
	o.var(r.sample_count);
	o.bytes(r.samples.b.data(), r.samples.b.size());
	write_chunk(r, CREA, r.sample_tick, r.sample_last, o.b);
	r.samples.b.clear();
	r.sample_count = 0;
	r.previous_sample.clear();
}

void flush_events(Recorder& r) {
	if (r.events.empty()) {
		return;
	}
	Out o;
	o.var(r.events.size());
	for (const auto& e : r.events) {
		o.var(e.tick);
		o.u8(e.kind);
		o.u32(e.creature);
		o.str(e.text);
	}
	write_chunk(r, EVNT, r.events.front().tick, r.events.back().tick, o.b);
	r.events.clear();
}

void write_world(Recorder& r, uint32_t tick) {
	try {
		auto sfc = sfc_dump_everything();
		VectorWriter w;
		sfc::write_sfc_v1_file(w, sfc);
		write_chunk(r, SNAP, tick, tick, w.vector());
	} catch (const std::exception& e) {
		fmt::print("WARN [Recording] couldn't save the world into the recording: {}\n", e.what());
	}
	r.last_world = tick;
}

void write_view_frame(Recorder& r, uint32_t tick) {
	if (r.view_frames == 0) {
		r.view_tick = tick;
	}
	Out& o = r.view;
	o.var(tick - r.view_tick);
	// what changed
	std::vector<const Item*> changed;
	std::vector<Key> removed;
	auto what_changed = [](const Item& item, const Item* was) -> uint8_t {
		if (!was) {
			return 32 | 1 | 2 | 4 | 8 | 16;
		}
		return static_cast<uint8_t>((item.asset != was->asset ? 1 : 0) | (item.frame != was->frame ? 2 : 0) |
									(sixteenths(item.x) != sixteenths(was->x) ? 4 : 0) | (sixteenths(item.y) != sixteenths(was->y) ? 8 : 0) |
									(item.z != was->z ? 16 : 0));
	};
	for (const auto& [key, item] : r.now) {
		auto it = r.last.find(key);
		if (what_changed(item, it == r.last.end() ? nullptr : &it->second)) {
			changed.push_back(&item);
		}
	}
	for (const auto& [key, item] : r.last) {
		if (!r.now.count(key)) {
			removed.push_back(key);
		}
	}
	o.var(changed.size());
	for (const Item* item : changed) {
		o.u32(item->owner);
		o.var(item->part);
		auto it = r.last.find(Key{item->owner, item->part});
		const Item* was = it == r.last.end() ? nullptr : &it->second;
		const uint8_t what = what_changed(*item, was);
		o.u8(what);
		if (what & 1) {
			o.var(item->asset);
		}
		if (what & 2) {
			o.var(static_cast<uint32_t>(item->frame));
		}
		if (what & 4) {
			o.svar(sixteenths(item->x) - (was ? sixteenths(was->x) : 0));
		}
		if (what & 8) {
			o.svar(sixteenths(item->y) - (was ? sixteenths(was->y) : 0));
		}
		if (what & 16) {
			o.svar(static_cast<int64_t>(item->z) - (was ? was->z : 0));
		}
	}
	o.var(removed.size());
	for (const auto& key : removed) {
		o.u32(key.owner);
		o.var(key.part);
	}
	std::vector<const Owner*> owners;
	for (const auto& [uid, owner] : r.owners_now) {
		if (r.owners_in_run.insert(uid).second) {
			owners.push_back(&owner);
		}
	}
	o.var(owners.size());
	for (const Owner* owner : owners) {
		o.u32(owner->uid);
		o.u8(owner->kind);
		o.u8(owner->family);
		o.u8(owner->genus);
		o.u8(owner->species);
	}
	// kept as written, so the next tick's changes are from what a reader has
	for (const Item* item : changed) {
		r.last[Key{item->owner, item->part}] = *item;
	}
	for (const auto& key : removed) {
		r.last.erase(key);
	}
	r.view_last = tick;
	if (++r.view_frames >= VIEW_RUN || tick - r.view_tick >= VIEW_RUN * 4) {
		flush_view(r);
		// with what happened, so a recording cut short keeps it
		flush_events(r);
	}
}

Sample sample_of(Creature& c) {
	Sample s;
	s.uid = uid_of(c);
	s.moniker = c.moniker;
	s.name = c.history_name;
	s.sex = c.sex;
	s.genus = static_cast<uint8_t>(c.genus);
	s.life_stage = c.age;
	s.flags = static_cast<uint8_t>((c.asleep ? Sample::ASLEEP : 0) | (c.dead ? Sample::DEAD : 0) |
								   (c.uid == g_engine_context.pointer->m_carried_object ? Sample::HELD : 0));
	s.expression = c.expression;
	s.health = static_cast<uint8_t>(std::clamp(creature_health_percent(c), 0, 255));
	s.gestation = static_cast<uint8_t>(std::clamp(creature_gestation_stage(c), 0, 255));
	const auto box = c.get_bbox();
	s.x = box.x, s.y = box.y, s.width = box.width, s.height = box.height;
	s.age_ticks = c.age_ticks;
	s.decision = c.current_decision;
	if (const Object* attention = g_engine_context.objects->try_get(c.attention)) {
		s.doing_to = get_object_name(attention);
		s.attention = uid_of(*attention);
	}
	s.has_target = c.has_target;
	s.target_x = c.target_x;
	s.target_y = c.target_y;
	if (c.biochemistry) {
		for (size_t i = 0; i < 256; ++i) {
			s.chemicals[i] = c.biochemistry->chemicals[i].concentration;
		}
	}
	if (c.brain) {
		for (const auto& lobe : c.brain->lobes) {
			s.lobe_sizes.push_back(static_cast<uint16_t>(lobe.num_neurons));
		}
		s.neurons.reserve(c.brain->neurons.size());
		for (const auto& n : c.brain->neurons) {
			s.neurons.push_back(n.output);
		}
	}
	return s;
}

void write_samples(Recorder& r, uint32_t tick, const std::vector<Sample>& creatures) {
	if (r.sample_count == 0) {
		r.sample_tick = tick;
	}
	Out& o = r.samples;
	o.var(tick - r.sample_tick);
	o.var(creatures.size());
	for (const auto& s : creatures) {
		Out body;
		write_sample(body, s);
		o.u32(s.uid);
		auto& before = r.previous_sample[s.uid];
		if (before.size() == body.b.size()) {
			o.u8(1);
			for (size_t i = 0; i < body.b.size(); ++i) {
				o.u8(static_cast<uint8_t>(body.b[i] ^ before[i]));
			}
		} else {
			o.u8(0);
			o.var(body.b.size());
			o.bytes(body.b.data(), body.b.size());
		}
		before = std::move(body.b);
	}
	r.sample_last = tick;
	if (++r.sample_count >= SAMPLES_PER_RUN) {
		flush_samples(r);
	}
}

// births, deaths, eggs, pregnancies and speech since the last tick recorded
void notice_events(Recorder& r, uint32_t tick) {
	auto add = [&](uint8_t kind, uint32_t who, std::string text) { r.events.push_back({tick, kind, who, std::move(text)}); };
	auto name_of = [](const Creature& c) { return c.history_name.empty() ? c.moniker : c.history_name; };
	std::vector<const Creature*> all;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature()) {
			all.push_back(c);
			const uint32_t uid = uid_of(*c);
			if (r.seen_creatures.insert(uid).second && r.started) {
				add(Event::APPEARED, uid, name_of(*c) + " appeared");
			}
			if (c->dead && r.dead_creatures.insert(uid).second && r.started) {
				add(Event::DIED, uid, name_of(*c) + " died");
			}
			const bool expecting = creature_gestation_stage(*c) > 0;
			if (expecting && r.pregnant.insert(uid).second && r.started) {
				add(Event::PREGNANT, uid, name_of(*c) + " is pregnant");
			} else if (!expecting) {
				r.pregnant.erase(uid);
			}
		} else if (obj->family == 2 && obj->genus == 5 && obj->species != 6) {
			if (r.seen_eggs.insert(uid_of(*obj)).second && r.started) {
				add(Event::EGG, 0, "An egg appeared");
			}
		} else if (const auto* bubble = obj->as_bubble()) {
			auto& said = r.bubbles[uid_of(*obj)];
			if (!bubble->text.empty() && bubble->text != said) {
				said = bubble->text;
				const Object* speaker = g_engine_context.objects->try_get(bubble->creator);
				const Creature* who = speaker ? speaker->as_creature() : nullptr;
				add(Event::SAID, who ? uid_of(*who) : 0, (who ? name_of(*who) : std::string("Someone")) + ": " + bubble->text);
			}
		}
	}
	if (!r.started) {
		add(Event::STARTED, 0, "Recording started");
		r.started = true;
	}
	if (r.events.size() >= 64 || (!r.events.empty() && tick - r.events.front().tick >= WORLD_EVERY)) {
		flush_events(r);
	}
}

void write_header(Recorder& r) {
	r.out.write(MAGIC, 8);
	Out v;
	v.u32(VERSION);
	v.u32(0);
	r.out.write(reinterpret_cast<const char*>(v.b.data()), 8);
	r.offset = 16;
	const uint32_t tick = g_engine_context.world->tick;
	Out info;
	char when[64] = {};
	const std::time_t now = std::time(nullptr);
	std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
	const std::vector<std::pair<std::string, std::string>> entries{
		{"world", fs::path(get_world_file_name()).stem().string()},
		{"recorded", when},
		{"first tick", std::to_string(tick)},
		{"world width", std::to_string(CREATURES1_WORLD_WIDTH)},
		{"world height", std::to_string(CREATURES1_WORLD_HEIGHT)},
		{"wrap width", std::to_string(g_engine_context.map->get_world_wrap_width())},
		{"made by", "opencreatures1"},
	};
	info.var(entries.size());
	for (const auto& [k, value] : entries) {
		info.str(k);
		info.str(value);
	}
	write_chunk(r, INFO, tick, tick, info.b);
	Out map;
	const auto& rooms = g_engine_context.map->get_rooms();
	map.var(rooms.size());
	for (const auto& room : rooms) {
		for (int32_t v : {room.left, room.top, room.right, room.bottom, room.type}) {
			map.svar(v);
		}
	}
	for (auto g : g_engine_context.map->get_groundlevel()) {
		map.var(g);
	}
	write_chunk(r, MAP, tick, tick, map.b);
	asset_for(r, g_engine_context.map->get_background(), tick);
}

void finish(Recorder& r) {
	try {
		flush_view(r);
		flush_samples(r);
		flush_events(r);
		Out o;
		o.var(r.index.size());
		for (const auto& c : r.index) {
			o.u32(c.type);
			o.var(c.tick);
			o.var(c.last);
			o.var(c.offset);
		}
		const uint64_t at = r.offset;
		write_chunk(r, INDX, r.first_tick, r.last_tick, o.b);
		Out end;
		end.bytes(reinterpret_cast<const uint8_t*>(END), 8);
		end.u64(at);
		r.out.write(reinterpret_cast<const char*>(end.b.data()), static_cast<std::streamsize>(end.b.size()));
		r.out.close();
		fmt::print("* Recording saved: {} ({} ticks)\n", r.path.string(), r.last_tick - r.first_tick);
	} catch (const std::exception& e) {
		fmt::print(stderr, "WARN [Recording] couldn't finish {}: {}\n", r.path.string(), e.what());
	}
}

} // namespace

bool g_capturing = false;

bool recording() {
	return s_rec != nullptr;
}

const fs::path& file() {
	return s_rec ? s_rec->path : s_no_file;
}

uint32_t ticks_recorded() {
	return s_rec && s_rec->started ? s_rec->last_tick - s_rec->first_tick : 0;
}

uint64_t bytes_written() {
	return s_rec ? s_rec->offset : 0;
}

void start(const fs::path& path) {
	stop();
	auto r = std::make_unique<Recorder>();
	r->path = path;
	fs::create_directories(path.parent_path());
	r->out.open(path, std::ios::binary | std::ios::trunc);
	if (!r->out) {
		throw Exception(fmt::format("Couldn't create the recording {}", path.string()));
	}
	r->generation = g_engine_context.generation();
	r->first_tick = r->last_tick = g_engine_context.world->tick;
	write_header(*r);
	write_world(*r, r->first_tick);
	s_rec = std::move(r);
	fmt::print("* Recording to {}\n", path.string());
}

void stop() {
	if (s_rec) {
		finish(*s_rec);
		s_rec.reset();
	}
	g_capturing = false;
}

bool begin_capture() {
	g_capturing = false;
	if (!s_rec) {
		return false;
	}
	// another world loaded: this recording is over
	if (s_rec->generation != g_engine_context.generation()) {
		stop();
		return false;
	}
	const uint32_t tick = g_engine_context.world->tick;
	if (s_rec->started && tick == s_rec->last_tick) {
		return false;
	}
	s_rec->now.clear();
	s_rec->owners_now.clear();
	g_capturing = true;
	return true;
}

void set_owner(const Object& o) {
	if (!s_rec) {
		return;
	}
	s_rec->owner = uid_of(o);
	s_rec->part = 0;
	s_rec->creature = o.as_creature();
	Owner owner;
	owner.uid = s_rec->owner;
	owner.family = static_cast<uint8_t>(o.family);
	owner.genus = static_cast<uint8_t>(o.genus);
	owner.species = static_cast<uint8_t>(o.species);
	owner.kind = o.as_creature() ? Owner::CREATURE : o.uid == g_engine_context.pointer->m_pointer_tool ? Owner::HAND :
				 (o.family == 2 && o.genus == 5 && o.species != 6) ? Owner::EGG : Owner::OBJECT;
	s_rec->owners_now[owner.uid] = owner;
}

void note(const ImageGallery& gallery, int32_t frame, float x, float y, int32_t z) {
	if (!s_rec || !g_capturing) {
		return;
	}
	try {
		const uint32_t asset = asset_for(*s_rec, gallery, g_engine_context.world->tick);
		if (asset == ~0u) {
			return;
		}
		Item item;
		item.owner = s_rec->owner;
		item.part = s_rec->part++;
		item.asset = asset;
		item.frame = frame;
		item.x = x;
		item.y = y;
		item.z = z;
		s_rec->now[Key{item.owner, item.part}] = item;
	} catch (const std::exception& e) {
		fmt::print(stderr, "WARN [Recording] stopped: {}\n", e.what());
		s_rec.reset();
		g_capturing = false;
	}
}

void end_capture() {
	if (!s_rec || !g_capturing) {
		return;
	}
	g_capturing = false;
	auto& r = *s_rec;
	const uint32_t tick = g_engine_context.world->tick;
	try {
		write_view_frame(r, tick);
		std::vector<Sample> creatures;
		if (!r.sampled || tick - r.last_sample >= SAMPLE_EVERY) {
			for (auto* obj : *g_engine_context.objects) {
				if (auto* c = obj->as_creature()) {
					creatures.push_back(sample_of(*c));
				}
			}
			write_samples(r, tick, creatures);
			r.last_sample = tick;
			r.sampled = true;
		}
		notice_events(r, tick);
		if (tick - r.last_world >= WORLD_EVERY) {
			flush_events(r);
			write_world(r, tick);
		}
		r.last_tick = tick;
	} catch (const std::exception& e) {
		fmt::print(stderr, "WARN [Recording] stopped: {}\n", e.what());
		r.out.close();
		s_rec.reset();
	}
}

// --- reading ---

Player::Player(const fs::path& file)
	: m_file(file) {
	std::ifstream in(file, std::ios::binary);
	if (!in) {
		throw Exception(fmt::format("Couldn't open {}", file.string()));
	}
	m_bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	if (m_bytes.size() < 16 || std::memcmp(m_bytes.data(), MAGIC, 8) != 0) {
		throw Exception(fmt::format("{} isn't a recording", file.filename().string()));
	}
	In header{m_bytes.data() + 8, m_bytes.data() + 16};
	m_version = header.u32();
	if (m_version > VERSION) {
		throw Exception("This recording was made by a newer version of the game");
	}
	// the chunks: from the index if the recording was finished, otherwise one by one
	std::vector<Chunk> chunks;
	bool indexed = false;
	if (m_bytes.size() >= 32 && std::memcmp(m_bytes.data() + m_bytes.size() - 16, END, 8) == 0) {
		In end{m_bytes.data() + m_bytes.size() - 8, m_bytes.data() + m_bytes.size()};
		const uint64_t at = end.u64();
		if (at + 16 <= m_bytes.size()) {
			In head{m_bytes.data() + at, m_bytes.data() + m_bytes.size()};
			if (head.u32() == INDX) {
				const auto raw = inflate(Chunk{INDX, 0, 0, at});
				In index{raw.data(), raw.data() + raw.size()};
				const auto n = index.var();
				for (uint64_t i = 0; i < n; ++i) {
					Chunk c;
					c.type = index.u32();
					c.tick = index.var32();
					c.last = index.var32();
					c.offset = index.var();
					chunks.push_back(c);
				}
				indexed = true;
			}
		}
	}
	if (!indexed) {
		uint64_t at = 16;
		while (at + 16 <= m_bytes.size()) {
			In head{m_bytes.data() + at, m_bytes.data() + m_bytes.size()};
			const uint32_t type = head.u32(), stored = head.u32();
			head.u32();
			const uint32_t tick = head.u32();
			if (at + 16 + stored > m_bytes.size() || std::memcmp(m_bytes.data() + at, END, 4) == 0) {
				break; // cut short in the middle of a chunk
			}
			uint32_t last = tick;
			if (type == VIEW || type == CREA || type == EVNT) {
				last = UINT32_MAX; // found when it's decoded
			}
			chunks.push_back({type, tick, last, at});
			at += 16 + stored;
		}
	}
	for (const auto& c : chunks) {
		if (c.type == INFO) {
			const auto raw = inflate(c);
			In i{raw.data(), raw.data() + raw.size()};
			const auto n = i.var();
			for (uint64_t k = 0; k < n; ++k) {
				auto key = i.str();
				m_info[key] = i.str();
			}
		} else if (c.type == ASET) {
			const auto raw = inflate(c);
			In i{raw.data(), raw.data() + raw.size()};
			Asset a;
			a.id = i.var32();
			a.name = i.str();
			if (m_version >= 2 && i.u8() == 1) {
				a.from_game = true;
				a.origin.absolute_base = static_cast<int32_t>(i.svar());
				a.absolute_base = a.origin.absolute_base;
				a.origin.image_count = static_cast<int32_t>(i.var());
				a.origin.background = a.background = i.u8() != 0;
				a.origin.remapped = i.u8() != 0;
				if (a.origin.remapped) {
					std::memcpy(a.origin.remap.data(), i.bytes(256), 256);
				}
				const auto files = static_cast<size_t>(i.var());
				for (size_t k = 0; k < files; ++k) {
					a.origin.files.push_back(i.str());
					a.md5.push_back(i.str());
				}
				if (m_assets.size() <= a.id) {
					m_assets.resize(a.id + 1);
				}
				m_assets[a.id] = std::move(a);
				continue;
			}
			a.absolute_base = static_cast<int32_t>(i.svar());
			a.background = i.u8() != 0;
			a.offsets.resize(static_cast<size_t>(i.var()));
			for (auto& v : a.offsets) {
				v = static_cast<int32_t>(i.svar());
			}
			const auto count = static_cast<size_t>(i.var());
			shared_array<Color> palette;
			std::vector<uint8_t> palette_bytes;
			for (size_t k = 0; k < count; ++k) {
				Image image;
				image.width = static_cast<int32_t>(i.var());
				image.height = static_cast<int32_t>(i.var());
				image.format = static_cast<imageformat>(i.u8());
				const size_t n = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * bytes_per_pixel(image.format);
				const uint8_t* pixels = i.bytes(n);
				image.data = shared_array<uint8_t>(pixels, pixels + n);
				const uint8_t* key = i.bytes(4);
				image.colorkey = Color{key[0], key[1], key[2], key[3]};
				if (image.format == if_index8) {
					const uint8_t* p = i.bytes(1024);
					// most of a file's images share one palette
					if (!palette || std::memcmp(palette_bytes.data(), p, 1024) != 0) {
						palette = shared_array<Color>(256);
						for (size_t c2 = 0; c2 < 256; ++c2) {
							palette[static_cast<ptrdiff_t>(c2)] = Color{p[c2 * 4], p[c2 * 4 + 1], p[c2 * 4 + 2], p[c2 * 4 + 3]};
						}
						palette_bytes.assign(p, p + 1024);
					}
					image.palette = palette;
				}
				a.images.push_back(std::move(image));
			}
			if (m_assets.size() <= a.id) {
				m_assets.resize(a.id + 1);
			}
			m_assets[a.id] = std::move(a);
		} else if (c.type == EVNT) {
			const auto raw = inflate(c);
			In i{raw.data(), raw.data() + raw.size()};
			const auto n = i.var();
			for (uint64_t k = 0; k < n; ++k) {
				Event e;
				e.tick = i.var32();
				e.kind = i.u8();
				e.creature = i.u32();
				e.text = i.str();
				m_events.push_back(std::move(e));
			}
		} else if (c.type == VIEW) {
			m_views.push_back(c);
		} else if (c.type == CREA) {
			m_samples.push_back(c);
		} else if (c.type == SNAP) {
			m_worlds.push_back(c);
		}
	}
	if (m_views.empty()) {
		throw Exception(fmt::format("{} has nothing recorded in it", file.filename().string()));
	}
	m_first = m_views.front().tick;
	// the last tick: the last run's, decoded if the recording wasn't finished
	if (m_views.back().last == UINT32_MAX) {
		const auto& frames = decode_view(m_views.back());
		m_views.back().last = frames.empty() ? m_views.back().tick : frames.back().first;
	}
	m_last = m_views.back().last;
	for (size_t k = 0; k + 1 < m_views.size(); ++k) {
		if (m_views[k].last == UINT32_MAX) {
			m_views[k].last = m_views[k + 1].tick - 1;
		}
	}
	for (size_t k = 0; k < m_samples.size(); ++k) {
		if (m_samples[k].last == UINT32_MAX) {
			m_samples[k].last = k + 1 < m_samples.size() ? m_samples[k + 1].tick - 1 : m_last;
		}
	}
	std::sort(m_events.begin(), m_events.end(), [](const Event& a, const Event& b) { return a.tick < b.tick; });
}

std::vector<uint8_t> Player::inflate(const Chunk& c) const {
	In head{m_bytes.data() + c.offset, m_bytes.data() + m_bytes.size()};
	head.u32();
	const uint32_t stored = head.u32(), size = head.u32();
	head.u32();
	head.need(stored);
	std::vector<uint8_t> raw(size);
	uLongf got = size;
	if (uncompress(raw.data(), &got, head.p, stored) != Z_OK || got != size) {
		throw Exception("The recording is damaged (a chunk won't inflate)");
	}
	return raw;
}

const Asset* Player::background() const {
	for (const auto& a : m_assets) {
		if (a.background) {
			return &a;
		}
	}
	return nullptr;
}

const Player::Chunk* Player::run_for(const std::vector<Chunk>& runs, uint32_t tick) const {
	auto it = std::upper_bound(runs.begin(), runs.end(), tick, [](uint32_t t, const Chunk& c) { return t < c.tick; });
	if (it == runs.begin()) {
		return runs.empty() ? nullptr : &runs.front();
	}
	return &*(it - 1);
}

const Player::Frames& Player::decode_view(const Chunk& c) {
	for (auto& d : m_decoded) {
		if (d.offset == c.offset) {
			d.used = ++m_uses;
			return d.frames;
		}
	}
	// in place of the one used least lately
	Decoded& d = m_decoded[0].used <= m_decoded[1].used ? m_decoded[0] : m_decoded[1];
	d.offset = ~0ull;
	d.frames.clear();
	auto& frames = d.frames;
	const auto raw = inflate(c);
	In in{raw.data(), raw.data() + raw.size()};
	std::map<Key, Item> state;
	const auto count = in.var();
	for (uint64_t f = 0; f < count; ++f) {
		const uint32_t tick = c.tick + in.var32();
		const auto changed = in.var();
		for (uint64_t k = 0; k < changed; ++k) {
			Key key;
			key.owner = in.u32();
			key.part = in.var32();
			Item& item = state[key];
			item.owner = key.owner;
			item.part = key.part;
			const uint8_t what = in.u8();
			if (what & 32) {
				item = Item{key.owner, key.part, 0, 0, 0, 0, 0};
			}
			if (what & 1) {
				item.asset = in.var32();
			}
			if (what & 2) {
				item.frame = static_cast<int32_t>(in.var32());
			}
			if (what & 4) {
				item.x = static_cast<float>(sixteenths(item.x) + in.svar()) / 16;
			}
			if (what & 8) {
				item.y = static_cast<float>(sixteenths(item.y) + in.svar()) / 16;
			}
			if (what & 16) {
				item.z = static_cast<int32_t>(item.z + in.svar());
			}
		}
		const auto removed = in.var();
		for (uint64_t k = 0; k < removed; ++k) {
			Key key;
			key.owner = in.u32();
			key.part = in.var32();
			state.erase(key);
		}
		const auto owners = in.var();
		for (uint64_t k = 0; k < owners; ++k) {
			Owner o;
			o.uid = in.u32();
			o.kind = in.u8();
			o.family = in.u8();
			o.genus = in.u8();
			o.species = in.u8();
			m_owners[o.uid] = o;
		}
		std::vector<Item> items;
		items.reserve(state.size());
		for (const auto& [key, item] : state) {
			items.push_back(item);
		}
		frames.emplace_back(tick, std::move(items));
	}
	d.offset = c.offset;
	d.used = ++m_uses;
	return d.frames;
}

void Player::decode_creatures(const Chunk& c) {
	if (m_sample_offset == c.offset) {
		return;
	}
	const auto raw = inflate(c);
	In in{raw.data(), raw.data() + raw.size()};
	m_sampled.clear();
	std::map<uint32_t, std::vector<uint8_t>> previous;
	const auto count = in.var();
	for (uint64_t s = 0; s < count; ++s) {
		const uint32_t tick = c.tick + in.var32();
		const auto creatures = in.var();
		std::vector<Sample> samples;
		for (uint64_t k = 0; k < creatures; ++k) {
			const uint32_t uid = in.u32();
			auto& before = previous[uid];
			std::vector<uint8_t> body;
			if (in.u8()) {
				const uint8_t* x = in.bytes(before.size());
				body.resize(before.size());
				for (size_t i = 0; i < body.size(); ++i) {
					body[i] = static_cast<uint8_t>(x[i] ^ before[i]);
				}
			} else {
				const auto n = static_cast<size_t>(in.var());
				const uint8_t* p = in.bytes(n);
				body.assign(p, p + n);
			}
			In b{body.data(), body.data() + body.size()};
			Sample sample = read_sample(b, m_version);
			sample.uid = uid;
			samples.push_back(std::move(sample));
			before = std::move(body);
		}
		m_sampled.emplace_back(tick, std::move(samples));
	}
	m_sample_offset = c.offset;
}

const std::vector<Item>& Player::items_at(uint32_t tick) {
	const Chunk* run = run_for(m_views, tick);
	if (!run) {
		return m_none;
	}
	const auto& frames = decode_view(*run);
	auto it = std::upper_bound(frames.begin(), frames.end(), tick, [](uint32_t t, const auto& f) { return t < f.first; });
	if (it == frames.begin()) {
		return frames.empty() ? m_none : frames.front().second;
	}
	return (it - 1)->second;
}

void Player::items_between(double position, std::vector<Item>& out) {
	position = std::clamp(position, static_cast<double>(m_first), static_cast<double>(m_last));
	const uint32_t a = shown_tick(static_cast<uint32_t>(position));
	out = items_at(a);
	const uint32_t b = next_tick(a);
	if (b <= a || position <= a) {
		return;
	}
	const auto f = static_cast<float>(std::min(1.0, (position - a) / (b - a)));
	const auto& next = items_at(b);
	// both in owner order: walk them together
	size_t j = 0;
	for (auto& item : out) {
		while (j < next.size() && Key{next[j].owner, next[j].part} < Key{item.owner, item.part}) {
			j++;
		}
		if (j < next.size() && next[j].owner == item.owner && next[j].part == item.part) {
			const float dx = next[j].x - item.x, dy = next[j].y - item.y;
			// a jump (a move across the world, or round its edge) isn't smoothed
			if (std::abs(dx) < 48 && std::abs(dy) < 48) {
				item.x += dx * f;
				item.y += dy * f;
			}
		}
	}
}

uint32_t Player::shown_tick(uint32_t tick) {
	const Chunk* run = run_for(m_views, tick);
	if (!run) {
		return m_first;
	}
	const auto& frames = decode_view(*run);
	auto it = std::upper_bound(frames.begin(), frames.end(), tick, [](uint32_t t, const auto& f) { return t < f.first; });
	return it == frames.begin() ? (frames.empty() ? run->tick : frames.front().first) : (it - 1)->first;
}

uint32_t Player::next_tick(uint32_t tick) {
	if (tick >= m_last) {
		return m_last;
	}
	const Chunk* run = run_for(m_views, tick);
	const auto& frames = decode_view(*run);
	auto it = std::upper_bound(frames.begin(), frames.end(), tick, [](uint32_t t, const auto& f) { return t < f.first; });
	if (it != frames.end()) {
		return it->first;
	}
	// the next run's first
	auto next = std::upper_bound(m_views.begin(), m_views.end(), tick, [](uint32_t t, const Chunk& c) { return t < c.tick; });
	return next == m_views.end() ? m_last : next->tick;
}

uint32_t Player::previous_tick(uint32_t tick) {
	if (tick <= m_first) {
		return m_first;
	}
	const uint32_t shown = shown_tick(tick);
	if (shown < tick) {
		return shown;
	}
	return shown_tick(tick - 1);
}

const Owner* Player::owner(uint32_t uid) const {
	auto it = m_owners.find(uid);
	return it == m_owners.end() ? nullptr : &it->second;
}

const std::vector<Sample>& Player::creatures_at(uint32_t tick) {
	const Chunk* run = run_for(m_samples, tick);
	if (!run) {
		return m_nobody;
	}
	decode_creatures(*run);
	auto it = std::upper_bound(m_sampled.begin(), m_sampled.end(), tick, [](uint32_t t, const auto& s) { return t < s.first; });
	if (it == m_sampled.begin()) {
		return m_sampled.empty() ? m_nobody : m_sampled.front().second;
	}
	return (it - 1)->second;
}

std::vector<uint8_t> Player::world_at(uint32_t tick, uint32_t* at) {
	const Chunk* c = run_for(m_worlds, tick);
	if (!c) {
		return {};
	}
	if (at) {
		*at = c->tick;
	}
	return inflate(*c);
}

std::string md5_hex(const uint8_t* data, size_t size) {
	Md5 md5;
	md5.add(data, size);
	return md5.hex();
}

const char* life_stage_name(uint8_t stage) {
	static const char* names[] = {"Baby", "Child", "Adolescent", "Youth", "Adult", "Old", "Senile"};
	return stage < 7 ? names[stage] : "Unknown";
}

} // namespace Recording
