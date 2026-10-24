#include "UIImages.h"

#include "DayNight.h"

#include "EngineContext.h"
#include "ImageManager.h"
#include "MapManager.h"
#include "PathManager.h"
#include "fileformats/ImageUtils.h"
#include "common/backend/Backend.h"
#include "common/render/RenderSystem.h"
#include "objects/Creature.h"
#include "sdlbackend/SDLBackend.h"

#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <map>
#include <memory>
#include <vector>

namespace UIImages {

namespace {

ImTextureID texture_id(const Texture& texture) {
	return (ImTextureID)(intptr_t)texture.ptr.get();
}

} // namespace

bool sprite(const ImageGallery& gallery, int32_t frame, float height, ImU32 tint) {
	if (!gallery.texture || frame < 0 || frame >= gallery.size()) {
		return false;
	}
	const Rect2i& loc = gallery.texture_locations[static_cast<size_t>(frame)];
	const float tw = static_cast<float>(gallery.texture.width);
	const float th = static_cast<float>(gallery.texture.height);
	const float w = static_cast<float>(loc.width), h = static_cast<float>(loc.height);
	const float scale = height > 0 && h > 0 ? height / h : 1.0f;
	ImGui::Image(texture_id(gallery.texture), ImVec2(w * scale, h * scale),
		ImVec2(static_cast<float>(loc.x) / tw, static_cast<float>(loc.y) / th),
		ImVec2(static_cast<float>(loc.x + loc.width) / tw, static_cast<float>(loc.y + loc.height) / th),
		ImGui::ColorConvertU32ToFloat4(tint));
	return true;
}

bool sprite(const char* name, int32_t frame, float height, ImU32 tint) {
	try {
		const auto& gallery = g_engine_context.images->get_image(name, frame, 1, ImageManager::IMAGE_SPR);
		return sprite(gallery, 0, height, tint);
	} catch (std::exception&) {
		return false;
	}
}

namespace {

// Pictures are textures of one renderer. Classic mode's kit windows have renderers of their
// own, so each picture is made once per renderer that draws it.
SDL_Renderer* s_target = nullptr; // null: the main window's
std::map<std::string, std::unique_ptr<Bitmap>> s_bitmaps; // by renderer and key
std::vector<Texture> s_main_textures; // kept alive for the main window's textures
std::map<SDL_Renderer*, std::vector<SDL_Texture*>> s_other_textures;

std::string cache_key(const std::string& key) {
	return s_target ? fmt::format("{}|{}", static_cast<const void*>(s_target), key) : key;
}

std::unique_ptr<Bitmap> make_bitmap(const Image& image) {
    if (image.width <= 0 || image.height <= 0) return nullptr;
    const size_t width = static_cast<size_t>(image.width), height = static_cast<size_t>(image.height);
    const size_t bytes = image.format == if_index8 ? 1 : image.format == if_rgb24 || image.format == if_bgr24 ? 3 : 2;
    if (width > image.data.size() / bytes / height) return nullptr;
    const size_t count = width * height;
    if (image.format == if_index8) {
        if (!image.palette || image.palette.size() > 256) return nullptr;
        for (size_t i = 0; i < count; ++i) if (image.data[i] >= image.palette.size()) return nullptr;
    }
	auto bmp = std::make_unique<Bitmap>();
	bmp->width = image.width;
	bmp->height = image.height;
	SDL_Texture* sdl = nullptr;
	if (!s_target) {
		Texture texture = get_backend()->createTexture(image.width, image.height);
		get_backend()->updateTexture(texture, Rect2i{0, 0, image.width, image.height}, image);
		sdl = static_cast<SDL_Texture*>(texture.ptr.get());
		s_main_textures.push_back(texture);
	} else {
		// 8-bit pictures through their palette, see-through where the alpha is 0
		std::vector<uint8_t> rgba(count * 4);
		for (size_t i = 0; i < count; ++i) {
			Color c{0, 0, 0, 0};
			if (image.format == if_index8 && image.palette) {
				c = image.palette[image.data[i]];
			} else if (image.format == if_rgb565 || image.format == if_rgb555) {
				continue; // not used by the kits' pictures
			} else if (image.format == if_bgr24) {
				c = Color{image.data[i * 3 + 2], image.data[i * 3 + 1], image.data[i * 3], 255};
			} else if (image.format == if_rgb24) {
				c = Color{image.data[i * 3], image.data[i * 3 + 1], image.data[i * 3 + 2], 255};
			}
			rgba[i * 4] = c.r;
			rgba[i * 4 + 1] = c.g;
			rgba[i * 4 + 2] = c.b;
			rgba[i * 4 + 3] = c.a;
		}
		sdl = SDL_CreateTexture(s_target, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, image.width, image.height);
		if (!sdl) {
			return nullptr;
		}
        if (!SDL_UpdateTexture(sdl, nullptr, rgba.data(), image.width * 4)) { SDL_DestroyTexture(sdl); return nullptr; }
		s_other_textures[s_target].push_back(sdl);
	}
	SDL_SetTextureBlendMode(sdl, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(sdl, SDL_SCALEMODE_NEAREST);
	bmp->texture = (ImTextureID)(intptr_t)sdl;
	return bmp;
}

} // namespace

void world_changed() {
    s_bitmaps.clear();
    s_main_textures.clear();
}

void set_target_renderer(SDL_Renderer* renderer) {
	s_target = renderer;
}

void forget_renderer(SDL_Renderer* renderer) {
	const std::string prefix = fmt::format("{}|", static_cast<const void*>(renderer));
	for (auto it = s_bitmaps.begin(); it != s_bitmaps.end();) {
		it = it->first.rfind(prefix, 0) == 0 ? s_bitmaps.erase(it) : std::next(it);
	}
	for (SDL_Texture* t : s_other_textures[renderer]) {
		SDL_DestroyTexture(t);
	}
	s_other_textures.erase(renderer);
}

const Bitmap* bitmap(const std::string& path) {
	const std::string key = cache_key("file:" + path);
	auto it = s_bitmaps.find(key);
	if (it != s_bitmaps.end()) {
		return it->second.get();
	}
	std::unique_ptr<Bitmap> result;
	try {
		auto file = g_engine_context.paths->find_path(PATH_TYPE_MAIN, path);
		if (!file.empty()) {
			auto images = ImageUtils::ReadImage(file);
			if (!images.empty()) {
				Image image = images[0];
				if (image.format == if_index8 && image.palette) {
					shared_array<Color> palette(image.palette.size());
					for (size_t i = 0; i < image.palette.size(); ++i) {
						palette[i] = image.palette[i];
						palette[i].a = 255;
					}
					palette[0].a = 0;
					image.palette = palette;
					image.colorkey = palette[0];
				}
				result = make_bitmap(image);
			}
		}
	} catch (std::exception&) {
		result.reset();
	}
	auto* raw = result.get();
	s_bitmaps[key] = std::move(result);
	return raw;
}

// A bitmap resource from a Windows program in the game's folder, for example the toolbar's
// picture strip (resource 128 in Creatures.exe, batch 6 Q10). Reads the program's resource
// table directly: type 2 (bitmaps), the id, the first language.
const Bitmap* exe_bitmap(const std::string& program, int resource_id) {
	const std::string key = cache_key(fmt::format("exe:{}:{}", program, resource_id));
	auto it = s_bitmaps.find(key);
	if (it != s_bitmaps.end()) {
		return it->second.get();
	}
	std::unique_ptr<Bitmap> result;
	try {
		auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, program);
		std::ifstream in(path, std::ios::binary);
		std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		auto u16 = [&](size_t at) -> uint32_t { return at + 2 <= d.size() ? static_cast<uint32_t>(d[at] | d[at + 1] << 8) : 0; };
		auto u32 = [&](size_t at) -> uint32_t { return at + 4 <= d.size() ? static_cast<uint32_t>(d[at] | d[at + 1] << 8 | d[at + 2] << 16 | static_cast<uint32_t>(d[at + 3]) << 24) : 0; };
		const size_t pe = u32(0x3c);
		if (d.size() < 64 || u32(pe) != 0x4550) {
			throw std::runtime_error("not a Windows program");
		}
		const size_t coff = pe + 4;
		const uint32_t sections = u16(coff + 2);
		const size_t optional = coff + 20;
		const uint32_t resource_rva = u32(optional + 96 + 8 * 2); // data directory 2 (PE32)
		const size_t section_table = optional + u16(coff + 16);
		auto file_offset = [&](uint32_t rva) -> size_t {
			for (uint32_t i = 0; i < sections; ++i) {
				const size_t sec = section_table + 40 * i;
				const uint32_t va = u32(sec + 12), size = std::max(u32(sec + 8), u32(sec + 16));
				if (rva >= va && rva < va + size) {
					return rva - va + u32(sec + 20);
				}
			}
			return 0;
		};
		const size_t root = file_offset(resource_rva);
		// the entry with this id in a resource directory (0 for the first), its offset from the root
		auto find = [&](size_t dir, uint32_t id, bool first) -> uint32_t {
			const uint32_t named = u16(dir + 12), ids = u16(dir + 14);
			for (uint32_t i = 0; i < named + ids; ++i) {
				const size_t e = dir + 16 + 8 * i;
				if (first ? i >= named : (u32(e) == id)) {
					return u32(e + 4);
				}
			}
			throw std::runtime_error("resource not found");
		};
		const uint32_t types = find(root, 2, false) & 0x7fffffff;
		const uint32_t names = find(root + types, static_cast<uint32_t>(resource_id), false) & 0x7fffffff;
		const uint32_t leaf = find(root + names, 0, true) & 0x7fffffff;
		const size_t dib = file_offset(u32(root + leaf));
		// a BITMAPINFOHEADER, its palette and the rows, bottom row first
		const int32_t width = static_cast<int32_t>(u32(dib + 4));
		const int32_t height = static_cast<int32_t>(u32(dib + 8));
		const uint32_t bits = u16(dib + 14);
		uint32_t colours = u32(dib + 32);
		if (colours == 0 && bits <= 8) {
			colours = 1u << bits;
		}
		const size_t palette_at = dib + u32(dib);
		const size_t pixels_at = palette_at + 4 * colours;
		const size_t stride = ((static_cast<size_t>(width) * bits + 31) / 32) * 4;
		if (width <= 0 || height <= 0 || (bits != 4 && bits != 8) || pixels_at + stride * static_cast<size_t>(height) > d.size()) {
			throw std::runtime_error("unsupported bitmap");
		}
		Image image;
		image.width = width;
		image.height = height;
		image.format = if_index8;
		image.data = shared_array<uint8_t>(static_cast<size_t>(width * height));
		image.palette = shared_array<Color>(256);
		for (uint32_t i = 0; i < 256; ++i) {
			image.palette[i] = i < colours ? Color{d[palette_at + 4 * i + 2], d[palette_at + 4 * i + 1], d[palette_at + 4 * i], 255} : Color{0, 0, 0, 255};
		}
		for (int32_t y = 0; y < height; ++y) {
			const size_t row = pixels_at + stride * static_cast<size_t>(height - 1 - y);
			for (int32_t x = 0; x < width; ++x) {
				const uint8_t v = bits == 8 ? d[row + static_cast<size_t>(x)] : static_cast<uint8_t>((d[row + static_cast<size_t>(x) / 2] >> (x % 2 ? 0 : 4)) & 15);
				image.data[static_cast<size_t>(y * width + x)] = v;
			}
		}
		result = make_bitmap(image);
	} catch (std::exception& e) {
		fmt::print("WARN [UIImages] couldn't read bitmap {} from {}: {}\n", resource_id, program, e.what());
	}
	auto* raw = result.get();
	s_bitmaps[key] = std::move(result);
	return raw;
}

const Bitmap* indexed_image(const std::string& image_key, int32_t width, int32_t height, const std::vector<uint8_t>& pixels) {
	const std::string key = cache_key("indexed:" + image_key);
	auto it = s_bitmaps.find(key);
	if (it != s_bitmaps.end()) {
		return it->second.get();
	}
	if (width <= 0 || height <= 0 || pixels.size() < static_cast<size_t>(width * height)) {
		return nullptr;
	}
	const auto& game_palette = g_engine_context.images->default_palette();
	Image image;
	image.width = width;
	image.height = height;
	image.format = if_index8;
	image.data = shared_array<uint8_t>(static_cast<size_t>(width * height));
	std::copy_n(pixels.begin(), width * height, image.data.data());
	image.palette = shared_array<Color>(256);
	for (size_t i = 0; i < 256; ++i) {
		image.palette[i] = i < game_palette.size() ? game_palette[i] : Color{0, 0, 0, 255};
		image.palette[i].a = 255;
	}
	image.palette[0].a = 0;
	image.colorkey = image.palette[0];
	auto bmp = make_bitmap(image);
	auto* raw = bmp.get();
	s_bitmaps[key] = std::move(bmp);
	return raw;
}

void draw_bitmap(ImDrawList* draw, const Bitmap* bmp, ImVec2 pos, float scale, Rect2i part) {
	if (!bmp) {
		return;
	}
	if (part.width <= 0 || part.height <= 0) {
		part = Rect2i{0, 0, bmp->width, bmp->height};
	}
	const float w = static_cast<float>(bmp->width), h = static_cast<float>(bmp->height);
	draw->AddImage(bmp->texture, pos, ImVec2(pos.x + static_cast<float>(part.width) * scale, pos.y + static_cast<float>(part.height) * scale),
		ImVec2(static_cast<float>(part.x) / w, static_cast<float>(part.y) / h),
		ImVec2(static_cast<float>(part.x + part.width) / w, static_cast<float>(part.y + part.height) / h));
}

bool draw_creature_face(ImDrawList* draw, const Creature& c, ImVec2 min, ImVec2 max, ImU32 tint) {
	if (!c.head) {
		return false;
	}
	return draw_face(draw, c.head->gallery(), c.head->base() + 8, min, max, tint); // looking at the camera
}

bool draw_face(ImDrawList* draw, const ImageGallery& gallery, int32_t frame, ImVec2 min, ImVec2 max, ImU32 tint) {
	if (!gallery.texture || frame < 0 || frame >= gallery.size()) {
		return false;
	}
	// just the visible part of the frame, without its empty margins
	Rect2i loc = gallery.texture_locations[static_cast<size_t>(frame)];
	if (static_cast<size_t>(frame) < gallery.content_bounds.size()) {
		const Rect2i& b = gallery.content_bounds[static_cast<size_t>(frame)];
		loc = Rect2i{loc.x + b.x, loc.y + b.y, b.width, b.height};
	}
	const float w = static_cast<float>(loc.width), h = static_cast<float>(loc.height);
	const float box_w = max.x - min.x, box_h = max.y - min.y;
	if (w <= 0 || h <= 0 || box_w <= 0 || box_h <= 0) {
		return false;
	}
	const float scale = std::min(box_w / w, box_h / h);
	const ImVec2 at(min.x + (box_w - w * scale) / 2, min.y + (box_h - h * scale) / 2);
	const float tw = static_cast<float>(gallery.texture.width), th = static_cast<float>(gallery.texture.height);
	draw->AddImage(texture_id(gallery.texture), at, ImVec2(at.x + w * scale, at.y + h * scale),
		ImVec2(static_cast<float>(loc.x) / tw, static_cast<float>(loc.y) / th),
		ImVec2(static_cast<float>(loc.x + loc.width) / tw, static_cast<float>(loc.y + loc.height) / th), tint);
	return true;
}

bool creature_portrait(const Creature& c, float height) {
	if (!c.head) {
		return false;
	}
	// head frames: 0-9 the views, 8 looking at the camera
	return sprite(c.head->gallery(), c.head->base() + 8, height);
}

struct WorldView::State {
	std::shared_ptr<RenderTarget> target;
	int32_t width = 0;
	int32_t height = 0;
	bool drawn = false;
};

void WorldView::look_at(Rect2i world_rect, int32_t width, int32_t height) {
	m_rect = world_rect;
	m_width = width;
	m_height = height;
	m_active = true;
}

void WorldView::render(RenderSystem& system) {
	if (!m_active || m_width <= 0 || m_height <= 0) {
		return;
	}
	m_active = false; // asked for again each frame it's shown
	if (!m_state) {
		m_state = new State;
	}
	if (!m_state->target || m_state->width != m_width || m_state->height != m_height) {
		m_state->target = get_backend()->newRenderTarget(m_width, m_height);
		// shown scaled up, so keep the pixels sharp
		if (auto* sdl = dynamic_cast<SDLRenderTarget*>(m_state->target.get())) {
			SDL_SetTextureScaleMode(sdl->texture, SDL_SCALEMODE_NEAREST);
		}
		m_state->width = m_width;
		m_state->height = m_height;
	}
	m_state->target->renderClear();
	RenderSystem::DrawConfig config;
	config.world_src = m_rect;
	config.world_wrap_width = g_engine_context.map->get_world_wrap_width();
	config.screen_dest = Rect2f{0, 0, static_cast<float>(m_width), static_cast<float>(m_height)};
	config.renderer = m_state->target.get();
	system.draw(config);
	// lit by the day and night, as the main view is (the modern layout)
	if (auto* sdl = dynamic_cast<SDLRenderTarget*>(m_state->target.get()); sdl && sdl->texture) {
		DayNight::render_view(SDL_GetRenderer(dynamic_cast<SDLBackend*>(get_backend())->window), sdl->texture, m_rect, m_width, m_height, this);
	}
	m_state->drawn = true;
}

void WorldView::image(ImVec2 size) {
	auto* sdl = m_state ? dynamic_cast<SDLRenderTarget*>(m_state->target.get()) : nullptr;
	if (!sdl || !m_state->drawn) {
		ImGui::Dummy(size);
		return;
	}
	ImGui::Image((ImTextureID)(intptr_t)sdl->texture, size);
}

bool WorldView::draw(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding) const {
	auto* sdl = m_state ? dynamic_cast<SDLRenderTarget*>(m_state->target.get()) : nullptr;
	if (!sdl || !m_state->drawn || m_state->width <= 0 || m_state->height <= 0) {
		return false;
	}
	// crop the view to the box's shape, keeping its middle
	const float box = (max.x - min.x) / std::max(1.f, max.y - min.y);
	const float view = static_cast<float>(m_state->width) / static_cast<float>(m_state->height);
	ImVec2 uv0(0, 0), uv1(1, 1);
	if (view > box) {
		const float keep = box / view;
		uv0.x = (1 - keep) / 2;
		uv1.x = 1 - uv0.x;
	} else {
		const float keep = view / box;
		uv0.y = (1 - keep) / 2;
		uv1.y = 1 - uv0.y;
	}
	list->AddImageRounded((ImTextureID)(intptr_t)sdl->texture, min, max, uv0, uv1, IM_COL32_WHITE, rounding);
	return true;
}

namespace {
bool s_photo_wanted = false;
Rect2i s_photo_rect;
PhotoCallback s_photo_callback = nullptr;
uint64_t s_photo_generation = 0;
} // namespace

void request_photo(Rect2i world_rect, PhotoCallback callback) {
    if (world_rect.width <= 0 || world_rect.height <= 0 || world_rect.width > 8352 || world_rect.height > 1200)
        throw Exception("Invalid photograph rectangle");
    s_photo_generation = g_engine_context.generation();
	s_photo_wanted = true;
	s_photo_rect = world_rect;
	s_photo_callback = callback;
}

void take_requested_photo(RenderSystem& system) {
    if (s_photo_generation != g_engine_context.generation()) { s_photo_wanted = false; s_photo_callback = nullptr; }
	if (!s_photo_wanted || !s_photo_callback) {
		return;
	}
	s_photo_wanted = false;
	auto target = get_backend()->newRenderTarget(s_photo_rect.width, s_photo_rect.height);
	auto* sdl = dynamic_cast<SDLRenderTarget*>(target.get());
	if (!sdl) {
		return;
	}
	target->renderClear();
	RenderSystem::DrawConfig config;
	config.world_src = s_photo_rect;
	config.world_wrap_width = g_engine_context.map->get_world_wrap_width();
	config.screen_dest = Rect2f{0, 0, static_cast<float>(s_photo_rect.width), static_cast<float>(s_photo_rect.height)};
	config.renderer = target.get();
	system.draw(config);

	// read the picture back and turn it into the game's palette
	SDL_Renderer* renderer = SDL_GetRendererFromTexture(sdl->texture);
	SDL_SetRenderTarget(renderer, sdl->texture);
	SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
	SDL_SetRenderTarget(renderer, nullptr);
	if (!surface) {
		return;
	}
	SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
	SDL_DestroySurface(surface);
	if (!rgba) {
		return;
	}
	const auto& palette = g_engine_context.images->default_palette();
	std::map<uint32_t, uint8_t> nearest;
	std::vector<uint8_t> pixels(static_cast<size_t>(rgba->w * rgba->h));
	for (int y = 0; y < rgba->h; ++y) {
		const auto* row = static_cast<const uint8_t*>(rgba->pixels) + y * rgba->pitch;
		for (int x = 0; x < rgba->w; ++x) {
			const uint8_t r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
			const uint32_t key = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
			auto found = nearest.find(key);
			if (found == nearest.end()) {
				// the closest colour, leaving out 0 (see-through)
				int best = 1, best_distance = 1 << 30;
				for (size_t i = 1; i < palette.size(); ++i) {
					const int dr = palette[i].r - r, dg = palette[i].g - g, db = palette[i].b - b;
					const int d = dr * dr + dg * dg + db * db;
					if (d < best_distance) {
						best_distance = d;
						best = static_cast<int>(i);
					}
				}
				found = nearest.emplace(key, static_cast<uint8_t>(best)).first;
			}
			pixels[static_cast<size_t>(y * rgba->w + x)] = found->second;
		}
	}
	const int32_t w = rgba->w, h = rgba->h;
	SDL_DestroySurface(rgba);
	s_photo_callback(w, h, pixels);
}

WorldView& creatures_view() {
	static WorldView view;
	return view;
}

} // namespace UIImages
