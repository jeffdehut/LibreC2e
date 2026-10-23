#include "PhotoAlbum.h"
#include "PersistentFile.h"

#include "EngineContext.h"
#include "ImageManager.h"
#include "PathManager.h"
#include "UIImages.h"
#include "ViewportManager.h"
#include "WorldSystems.h"
#include "common/io/FileWriter.h"
#include "objects/Creature.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <ctime>
#include <fmt/core.h>
#include <fstream>
#include <imgui.h>
#include <iterator>
#include <map>
#include <filesystem>

namespace fs = std::filesystem;

namespace PhotoAlbum {

namespace {

std::string s_pending_moniker; // the creature a photo is being taken of
std::map<std::string, int32_t> s_shown; // the photo shown in each album
std::string s_status;

std::string album_file(const std::string& moniker) {
	return moniker + ".openc1.photos";
}

// An album is in its world's Photos folder: the playing world's, or another world's (a Norn can
// move between worlds), or Photo Albums for one no world claims, or loose in the game folder
// (before the folders). A new one goes in the playing world's. Found paths are kept, as the
// kits ask every frame.
fs::path album_path(const std::string& moniker) {
	static std::map<std::string, fs::path> found;
	static uint64_t generation = ~0ull;
	if (generation != g_engine_context.generation()) {
		found.clear();
		generation = g_engine_context.generation();
	}
	if (auto it = found.find(moniker); it != found.end() && fs::exists(it->second)) {
		return it->second;
	}
	const auto file = album_file(moniker);
	const auto main = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "");
	std::vector<fs::path> places{current_world_folder() / "Photos" / file};
	std::error_code err;
	if (fs::is_directory(main / "Worlds", err)) {
		for (const auto& world : fs::directory_iterator(main / "Worlds", err)) {
			places.push_back(world.path() / "Photos" / file);
		}
	}
	places.push_back(main / "Photo Albums" / file);
	places.push_back(main / file);
	for (const auto& place : places) {
		if (fs::is_regular_file(place, err)) {
			return found[moniker] = place;
		}
	}
	return places.front();
}

void photo_taken(int32_t width, int32_t height, const std::vector<uint8_t>& pixels) {
	if (s_pending_moniker.empty()) {
		return;
	}
	auto photos = load(s_pending_moniker);
	Photo photo;
	photo.time = static_cast<uint32_t>(std::time(nullptr));
	photo.width = width;
	photo.height = height;
	photo.pixels = pixels;
	photos.push_back(photo);
    try { save(s_pending_moniker, photos); }
    catch (const std::exception& e) {
        s_status = e.what();
        s_pending_moniker.clear();
        return;
    }
	// a new photo goes at the end and is the one shown
	s_shown[s_pending_moniker] = static_cast<int32_t>(photos.size()) - 1;
	s_status = "Photo taken.";
	s_pending_moniker.clear();
}

} // namespace

void world_changed() {
    s_pending_moniker.clear(); s_shown.clear(); s_status.clear();
}

std::vector<Photo> load(const std::string& moniker) {
	std::vector<Photo> photos;
	const auto path = album_path(moniker);
	if (!fs::is_regular_file(path)) {
		return photos;
	}
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Exception("Couldn't open album " + path.string());
	std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (d.size() < 6 || std::string(d.begin(), d.begin() + 4) != "OC1P") {
		return photos;
	}
	size_t i = 4;
	auto u16 = [&]() { uint32_t v = d[i] | (d[i + 1] << 8); i += 2; return v; };
	const uint32_t count = u16();
	for (uint32_t n = 0; n < count && i + 8 <= d.size(); ++n) {
		Photo p;
		p.time = d[i] | (d[i + 1] << 8) | (d[i + 2] << 16) | (static_cast<uint32_t>(d[i + 3]) << 24);
		i += 4;
		p.width = static_cast<int32_t>(u16());
		p.height = static_cast<int32_t>(u16());
		const size_t size = static_cast<size_t>(p.width) * static_cast<size_t>(p.height);
		if (i + size > d.size()) {
			break;
		}
		p.pixels.assign(d.begin() + static_cast<long>(i), d.begin() + static_cast<long>(i + size));
		i += size;
		photos.push_back(std::move(p));
	}
	return photos;
}

void save(const std::string& moniker, const std::vector<Photo>& photos) {
	std::vector<uint8_t> out{'O', 'C', '1', 'P'};
	auto u16 = [&](uint32_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); };
	u16(static_cast<uint32_t>(photos.size()));
	for (const auto& p : photos) {
		u16(p.time & 0xffff);
		u16(p.time >> 16);
		u16(static_cast<uint32_t>(p.width));
		u16(static_cast<uint32_t>(p.height));
		out.insert(out.end(), p.pixels.begin(), p.pixels.end());
	}
    write_persistent_file(album_path(moniker), out.data(), out.size());
}

bool take_photo(Creature& c) {
	// "dde: panc": the view jumps to the creature, centred, its ground at 5/8 of the height
	auto& viewport = *g_engine_context.viewport;
	auto camera = viewport.get_main_camera_rect();
	viewport.set_scroll_position(static_cast<int32_t>(c.ground_x()) - camera.width / 2, static_cast<int32_t>(c.ground_y()) - camera.height * 5 / 8);
	viewport.update();
	camera = viewport.get_main_camera_rect();
	// "dde: pict": a 120 x 140 picture centred on the creature, which must be in the view
	auto b = c.get_bbox();
	Rect2i rect{static_cast<int32_t>(b.x + b.width / 2) - PHOTO_WIDTH / 2, static_cast<int32_t>(b.y + b.height / 2) - PHOTO_HEIGHT / 2, PHOTO_WIDTH, PHOTO_HEIGHT};
	int32_t rx = rect.x - camera.x;
	if (rx < -CREATURES1_WORLD_WIDTH / 2) {
		rx += CREATURES1_WORLD_WIDTH;
	} else if (rx > CREATURES1_WORLD_WIDTH / 2) {
		rx -= CREATURES1_WORLD_WIDTH;
	}
	const int32_t ry = rect.y - camera.y;
	if (rx < 0 || ry < 0 || rx + rect.width > camera.width || ry + rect.height > camera.height) {
		s_status = "Couldn't fit the creature in the picture.";
		return false;
	}
	s_pending_moniker = c.moniker;
	UIImages::request_photo(rect, photo_taken);
	return true;
}

bool take_photo_quietly(Creature& c) {
	// rendered off screen, so the view doesn't move; one photo at a time
	if (!s_pending_moniker.empty()) {
		return false;
	}
	auto b = c.get_bbox();
	Rect2i rect{static_cast<int32_t>(b.x + b.width / 2) - PHOTO_WIDTH / 2, static_cast<int32_t>(b.y + b.height / 2) - PHOTO_HEIGHT / 2, PHOTO_WIDTH, PHOTO_HEIGHT};
	s_pending_moniker = c.moniker;
	UIImages::request_photo(rect, photo_taken);
	return true;
}

std::string label(const Photo& photo) {
	std::time_t t = static_cast<std::time_t>(photo.time);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char text[64];
	std::strftime(text, sizeof(text), "%H:%M %d %B %Y", &tm);
	return text;
}

bool save_bmp(const Photo& photo, const std::string& file_name) {
	SDL_Surface* surface = SDL_CreateSurface(photo.width, photo.height, SDL_PIXELFORMAT_INDEX8);
	if (!surface) {
		return false;
	}
	const auto& game = g_engine_context.images->default_palette();
	std::vector<SDL_Color> colours(256);
	for (size_t i = 0; i < 256 && i < game.size(); ++i) {
		colours[i] = SDL_Color{game[i].r, game[i].g, game[i].b, 255};
	}
	SDL_SetPaletteColors(SDL_CreateSurfacePalette(surface), colours.data(), 0, 256);
	for (int y = 0; y < photo.height; ++y) {
		std::copy_n(photo.pixels.begin() + y * photo.width, photo.width, static_cast<uint8_t*>(surface->pixels) + y * surface->pitch);
	}
	const auto folder = current_world_folder() / "Photos";
	std::error_code err;
	fs::create_directories(folder, err);
	const bool ok = SDL_SaveBMP(surface, (folder / file_name).string().c_str());
	SDL_DestroySurface(surface);
	return ok;
}

int32_t draw(Creature* creature, const std::string& moniker, bool controls) {
    std::vector<Photo> photos;
    try { photos = load(moniker); }
    catch (const std::exception& e) {
        ImGui::TextWrapped("%s", e.what());
        return -1;
    }
	int32_t& shown = s_shown[moniker];
	shown = photos.empty() ? -1 : std::clamp(shown, 0, static_cast<int32_t>(photos.size()) - 1);

	// the album page (photograph.bmp) with the photo, or the empty slot (Blank.bmp)
	const auto* page = UIImages::bitmap("photograph.bmp");
	const float scale = 1.0f;
	ImVec2 pos = ImGui::GetCursorScreenPos();
	const float page_w = page ? static_cast<float>(page->width) : 312, page_h = page ? static_cast<float>(page->height) : 299;
	ImGui::Dummy(ImVec2(page_w * scale, page_h * scale));
	auto* draw = ImGui::GetWindowDrawList();
	UIImages::draw_bitmap(draw, page, pos, scale);
	// the opening in the gold frame, measured from photograph.bmp: 120 x 140 at (62, 32),
	// the size of a photo; pictures are centred in it
	constexpr float OPENING_X = 62, OPENING_Y = 32, OPENING_W = 120, OPENING_H = 140;
	auto centred = [&](float w, float h) {
		return ImVec2(pos.x + (OPENING_X + (OPENING_W - w) / 2) * scale, pos.y + (OPENING_Y + (OPENING_H - h) / 2) * scale);
	};
	if (shown >= 0) {
		const auto& p = photos[static_cast<size_t>(shown)];
		const auto* picture = UIImages::indexed_image(fmt::format("photo:{}:{}:{}", moniker, shown, p.time), p.width, p.height, p.pixels);
		UIImages::draw_bitmap(draw, picture, centred(static_cast<float>(p.width), static_cast<float>(p.height)), scale);
	} else if (const auto* blank = UIImages::bitmap("Blank.bmp")) {
		UIImages::draw_bitmap(draw, blank, centred(static_cast<float>(blank->width), static_cast<float>(blank->height)), scale);
	}

	if (ImGui::ArrowButton("##prevphoto", ImGuiDir_Left) && shown > 0) {
		shown--;
	}
	ImGui::SameLine();
	if (shown >= 0) {
		ImGui::Text("%d of %zu: %s", shown + 1, photos.size(), label(photos[static_cast<size_t>(shown)]).c_str());
	} else {
		ImGui::TextDisabled("No photographs yet");
	}
	ImGui::SameLine();
	if (ImGui::ArrowButton("##nextphoto", ImGuiDir_Right) && shown + 1 < static_cast<int32_t>(photos.size())) {
		shown++;
	}

	if (controls) {
		ImGui::BeginDisabled(!creature || creature->dead);
		if (ImGui::Button("Camera") && creature) {
			take_photo(*creature);
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Take a photo of your Norn");
		ImGui::SameLine();
		ImGui::BeginDisabled(shown < 0);
		if (ImGui::Button("Delete")) {
			ImGui::OpenPopup("Delete photograph");
		}
		ImGui::SameLine();
		static char file_name[64] = "";
		if (ImGui::Button("Save As")) {
			std::snprintf(file_name, sizeof(file_name), "%s photo %d.bmp", moniker.c_str(), shown + 1);
			ImGui::OpenPopup("Save photograph");
		}
		ImGui::EndDisabled();
		if (ImGui::BeginPopupModal("Delete photograph", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextUnformatted("Delete this photograph?");
			if (ImGui::Button("Delete", ImVec2(100, 0)) && shown >= 0) {
				photos.erase(photos.begin() + shown);
                try { save(moniker, photos); }
                catch (const std::exception& e) { s_status = e.what(); }
				shown = std::max(0, shown - 1);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		if (ImGui::BeginPopupModal("Save photograph", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextUnformatted("Bitmaps (*.bmp), saved in the game folder:");
			ImGui::InputText("##bmpname", file_name, sizeof(file_name));
			if (ImGui::Button("Save", ImVec2(100, 0)) && shown >= 0) {
				std::string name = file_name;
				if (name.size() < 4 || name.substr(name.size() - 4) != ".bmp") {
					name += ".bmp";
				}
				s_status = save_bmp(photos[static_cast<size_t>(shown)], name) ? "Saved " + name : "Couldn't save " + name;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		if (!s_status.empty()) {
			ImGui::TextDisabled("%s", s_status.c_str());
		}
	}
	return shown;
}

} // namespace PhotoAlbum
