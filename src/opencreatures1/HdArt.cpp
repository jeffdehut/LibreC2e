#include "HdArt.h"

#include "ImageGallery.h"
#include "common/Ascii.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <fmt/core.h>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace HdArt {

namespace {

// S32 files, as LibreCreatures reads them (adapted from its src/c1/display/s32.cpp, GNU GPL 3;
// see third_party/librecreatures/NOTICE.md). All little-endian:
//   u32 flags (bit 0x4 set: 32-bit), u16 image count,
//   per image: u32 offset of its PNG, u16 width, u16 height (not relied on),
//   the PNGs, each from its offset to the next one's, the last to the end of the file.
// A frame's real size is its PNG's; see-through is its alpha. Nothing in a file is trusted
// until it has been checked against the file's length.
constexpr uint32_t THIRTY_TWO_BIT = 0x4;
constexpr uint32_t MAX_DIMENSION = 16384;
constexpr uint64_t MAX_PIXELS = 16u * 1024u * 1024u;
constexpr uint64_t MAX_FILE_BYTES = 512u * 1024u * 1024u;
constexpr size_t PNG_MINIMUM = 8 + 4 + 4 + 13 + 4; // signature and IHDR

struct S32Frame {
	uint32_t offset = 0, size = 0, width = 0, height = 0;
};

enum class State : uint8_t { none, queued, ready, failed };

struct File {
	fs::path path;
	bool parsed = false, usable = false;
	std::vector<S32Frame> frames;
	std::vector<State> state;
	std::vector<Frame> ready;
	std::map<std::pair<int32_t, int32_t>, bool> ranges; // galleries (base, count) checked
};

std::map<std::string, File> s_files; // by sprite name, lower case ("eggs")
std::deque<std::pair<File*, uint32_t>> s_queue;
std::function<Texture(const std::vector<uint8_t>&, int32_t, int32_t)> s_loader;
bool s_enabled = false;
std::set<std::string> s_reported;

uint32_t le32(const uint8_t* p) {
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint32_t be32(const uint8_t* p) {
	return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

void report(const File& f, const std::string& why) {
	if (s_reported.insert(f.path.string() + why).second) {
		fmt::print("WARN [HdArt] {}: {}, ignored\n", f.path.filename().string(), why);
	}
}

// indexes a file without decoding any image: the flag, the count, every offset in order and
// inside the file, each PNG's signature and IHDR, and the size limits
bool parse(File& f) {
	std::error_code err;
	const auto size = fs::file_size(f.path, err);
	if (err || size < 6 || size > MAX_FILE_BYTES) {
		return false;
	}
	std::ifstream in(f.path, std::ios::binary);
	uint8_t header[6];
	if (!in.read(reinterpret_cast<char*>(header), 6) || (le32(header) & THIRTY_TWO_BIT) == 0) {
		return false;
	}
	const size_t count = size_t(header[4]) | size_t(header[5]) << 8;
	const uint64_t headers_end = 6 + uint64_t(count) * 8;
	if (count == 0 || headers_end > size) {
		return false;
	}
	std::vector<uint8_t> table(count * 8);
	if (!in.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()))) {
		return false;
	}
	f.frames.resize(count);
	for (size_t i = 0; i < count; ++i) {
		f.frames[i].offset = le32(table.data() + i * 8);
	}
	static const uint8_t signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	for (size_t i = 0; i < count; ++i) {
		const uint64_t start = f.frames[i].offset;
		const uint64_t end = i + 1 < count ? f.frames[i + 1].offset : size;
		if (start < headers_end || end > size || end < start || end - start < PNG_MINIMUM) {
			return false;
		}
		uint8_t png[24];
		in.seekg(static_cast<std::streamoff>(start));
		if (!in.read(reinterpret_cast<char*>(png), 24) || std::memcmp(png, signature, 8) != 0 || be32(png + 8) != 13 ||
			std::memcmp(png + 12, "IHDR", 4) != 0) {
			return false;
		}
		const uint32_t width = be32(png + 16), height = be32(png + 20);
		if (width < 1 || height < 1 || width > MAX_DIMENSION || height > MAX_DIMENSION || uint64_t(width) * height > MAX_PIXELS) {
			return false;
		}
		f.frames[i].size = static_cast<uint32_t>(end - start);
		f.frames[i].width = width;
		f.frames[i].height = height;
	}
	f.state.assign(count, State::none);
	f.ready.resize(count);
	return true;
}

File* file_for(const std::string& name) {
	if (!s_enabled || s_files.empty()) {
		return nullptr;
	}
	auto it = s_files.find(name);
	if (it == s_files.end()) {
		return nullptr;
	}
	File& f = it->second;
	if (!f.parsed) {
		f.parsed = true;
		f.usable = parse(f);
		if (!f.usable) {
			report(f, "not a valid S32 file");
		}
	}
	return f.usable ? &f : nullptr;
}

// Whether a file has frames [base, base + count) at exactly twice the sizes given. A frame
// with no size in the original is never drawn, so it isn't held against the file.
template <typename SizeOf>
bool frames_match(File& f, int32_t base, int32_t count, SizeOf size_of) {
	auto [it, fresh] = f.ranges.emplace(std::pair{base, count}, false);
	if (!fresh) {
		return it->second;
	}
	bool ok = base >= 0 && count > 0;
	for (int32_t i = 0; ok && i < count; ++i) {
		const auto [w, h] = size_of(i);
		if (w <= 0 || h <= 0) {
			continue;
		}
		const size_t frame = static_cast<size_t>(base + i);
		if (frame >= f.frames.size()) {
			report(f, fmt::format("has no frame {}", frame));
			ok = false;
		} else if (f.frames[frame].width != uint32_t(w) * 2 || f.frames[frame].height != uint32_t(h) * 2) {
			report(f, fmt::format("frame {} is {}x{}, not {}x{}", frame, f.frames[frame].width, f.frames[frame].height, w * 2, h * 2));
			ok = false;
		}
	}
	return it->second = ok;
}

const Frame* get(File& f, uint32_t frame) {
	switch (f.state[frame]) {
		case State::ready:
			return &f.ready[frame];
		case State::none:
			f.state[frame] = State::queued;
			s_queue.emplace_back(&f, frame);
			return nullptr;
		default:
			return nullptr;
	}
}

} // namespace

void set_folders(const std::vector<fs::path>& folders) {
	s_queue.clear();
	s_files.clear();
	std::error_code err;
	for (const auto& folder : folders) {
		if (folder.empty() || !fs::is_directory(folder, err)) {
			continue;
		}
		for (const auto& entry : fs::directory_iterator(folder, err)) {
			const auto file = entry.path().filename().string();
			const auto lower = to_ascii_lowercase(file);
			constexpr std::string_view suffix = "@2x.s32";
			if (!entry.is_regular_file(err) || lower.size() <= suffix.size() ||
				lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) != 0) {
				continue;
			}
			const auto name = lower.substr(0, lower.size() - suffix.size());
			if (!s_files.count(name)) {
				s_files[name].path = entry.path();
			}
		}
	}
	if (!s_files.empty()) {
		fmt::print("* Higher-resolution art: {} sprite files\n", s_files.size());
	}
}

void set_loader(std::function<Texture(const std::vector<uint8_t>&, int32_t, int32_t)> loader) {
	s_loader = std::move(loader);
}

void set_enabled(bool on) {
	s_enabled = on;
}

bool enabled() {
	return s_enabled && !s_files.empty();
}

size_t file_count() {
	return s_files.size();
}

const Frame* frame(const ImageGallery& gallery, int32_t index) {
	File* f = file_for(gallery.name);
	if (!f || !gallery.has_frame(index) ||
		!frames_match(*f, gallery.absolute_base, gallery.size(), [&](int32_t i) { return std::pair{gallery.width(i), gallery.height(i)}; })) {
		return nullptr;
	}
	return get(*f, static_cast<uint32_t>(gallery.absolute_base + index));
}

RenderItem render_item(const ImageGallery& gallery, int32_t index, float x, float y, int32_t z) {
	if (const Frame* hd = frame(gallery, index)) {
		RenderItem item = create_render_item_texture(hd->texture, hd->src, x, y, z);
		item.dest.width = static_cast<float>(gallery.width(index));
		item.dest.height = static_cast<float>(gallery.height(index));
		return item;
	}
	return create_render_item_texture(gallery.texture, gallery.texture_locations[static_cast<size_t>(index)], x, y, z);
}

const Frame* background_tile(int32_t index) {
	// back.spr: 58 columns of 8 tiles, 144 x 150 each
	constexpr int32_t TILES = 58 * 8;
	File* f = file_for("back");
	if (!f || index < 0 || index >= TILES || !frames_match(*f, 0, TILES, [](int32_t) { return std::pair{144, 150}; })) {
		return nullptr;
	}
	return get(*f, static_cast<uint32_t>(index));
}

void update(double budget_seconds) {
	if (s_queue.empty() || !s_loader) {
		return;
	}
	const auto start = std::chrono::steady_clock::now();
	while (!s_queue.empty() && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < budget_seconds) {
		auto [f, frame] = s_queue.front();
		s_queue.pop_front();
		const S32Frame& s = f->frames[frame];
		std::vector<uint8_t> png(s.size);
		std::ifstream in(f->path, std::ios::binary);
		in.seekg(s.offset);
		Texture texture;
		if (in.read(reinterpret_cast<char*>(png.data()), static_cast<std::streamsize>(png.size()))) {
			texture = s_loader(png, static_cast<int32_t>(s.width), static_cast<int32_t>(s.height));
		}
		if (texture) {
			f->ready[frame] = Frame{texture, Rect2i{0, 0, static_cast<int32_t>(s.width), static_cast<int32_t>(s.height)}};
			f->state[frame] = State::ready;
		} else {
			f->state[frame] = State::failed;
			report(*f, fmt::format("frame {} couldn't be decoded", frame));
		}
	}
}

} // namespace HdArt
