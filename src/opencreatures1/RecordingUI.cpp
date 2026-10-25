#include "RecordingUI.h"

#include "DayNight.h"
#include "EngineContext.h"
#include "FileDialogs.h"
#include "GameUI.h"
#include "HdArt.h"
#include "ImageManager.h"
#include "Kits.h"
#include "MapManager.h"
#include "Recording.h"
#include "UIArt.h"
#include "UIImages.h"
#include "UIStyle.h"
#include "ViewportManager.h"
#include "WorldSystems.h"
#include "common/render/RenderSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fmt/core.h>
#include <imgui.h>
#include <map>
#include <memory>

namespace fs = std::filesystem;

namespace RecordingUI {

namespace {

// a tick is 90 ms at the original's speed
constexpr double TICKS_PER_SECOND = 1000.0 / 90.0;
constexpr float SPEEDS[] = {0.25f, 0.5f, 1, 2, 4, 8, 16, 64};

std::unique_ptr<Recording::Player> s_player;
double s_position = 0; // the tick being shown, with its fraction while playing
int s_direction = 0; // 1 forwards, -1 backwards, 0 paused
int s_speed = 2; // in SPEEDS
bool s_scrubbing = false;
std::map<uint32_t, ImageGallery> s_galleries;
DayNight::Shown s_shown;
// the creature clicked on, and whether to keep it in view
uint32_t s_selected = 0;
bool s_details = false, s_follow = false, s_inspector = false, s_events = false;
ImVec2 s_details_at;
// this frame's picture (smoothed between recorded ticks), and where each owner is in it
std::vector<Recording::Item> s_items;
std::map<uint32_t, Rect2f> s_boxes;
// the selected creature's view: what it's looking at, and marking that in the main view
bool s_eyes = false, s_show_looking = true;
UIImages::WorldView s_eye, s_face; // what it's looking at, and its face
float s_eye_x = 0, s_eye_y = 0;
bool s_eye_placed = false;
constexpr int EYE_WIDTH = 128, EYE_HEIGHT = 96;
// sprites the recording names that this installation doesn't have
int s_missing = 0;

const ImageGallery& gallery_for(uint32_t asset) {
	static const ImageGallery none;
	auto it = s_galleries.find(asset);
	if (it != s_galleries.end()) {
		return it->second;
	}
	const auto& assets = s_player->assets();
	if (asset >= assets.size()) {
		return none;
	}
	const auto& a = assets[asset];
	if (a.from_game) {
		// the game's own sprite files, from this installation
		try {
			std::vector<int32_t> offsets;
			auto images = g_engine_context.images->load_images(a.origin, offsets);
			return s_galleries[asset] = make_gallery(a.name, a.absolute_base, offsets, images);
		} catch (const std::exception& e) {
			fmt::print("WARN [Recording] the sprite {:?} isn't here: {}\n", a.name, e.what());
			s_missing++;
			return s_galleries[asset] = ImageGallery{};
		}
	}
	if (a.images.empty()) {
		return none;
	}
	return s_galleries[asset] = make_gallery(a.name, a.absolute_base, a.offsets, a.images);
}

uint32_t tick_now() {
	return static_cast<uint32_t>(std::clamp(s_position, static_cast<double>(s_player->first_tick()), static_cast<double>(s_player->last_tick())));
}

const Recording::Sample* selected_sample() {
	for (const auto& s : s_player->creatures_at(tick_now())) {
		if (s.uid == s_selected) {
			return &s;
		}
	}
	return nullptr;
}

std::string name_of(const Recording::Sample& s) {
	return s.name.empty() ? s.moniker : s.name + " (" + s.moniker + ")";
}

// "1:02:03" of recording, at the original's speed
std::string duration(uint32_t ticks) {
	const auto seconds = static_cast<long>(static_cast<double>(ticks) / TICKS_PER_SECOND);
	return seconds >= 3600 ? fmt::format("{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60) : fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

std::string clock_at(uint32_t tick) {
	const float t = DayNight::time_of_day(tick);
	const int minutes = static_cast<int>(t * 24 * 60);
	return fmt::format("Day {}, {:02}:{:02}", DayNight::day_number(tick), minutes / 60, minutes % 60);
}

ImU32 event_colour(uint8_t kind) {
	switch (kind) {
		case Recording::Event::APPEARED: return IM_COL32(110, 200, 110, 255);
		case Recording::Event::DIED: return IM_COL32(200, 80, 70, 255);
		case Recording::Event::EGG: return IM_COL32(110, 170, 255, 255);
		case Recording::Event::PREGNANT: return IM_COL32(240, 130, 200, 255);
		default: return IM_COL32(200, 200, 200, 120);
	}
}

// a bar for a value 0-255, with its label
void level(const char* label, uint8_t value, ImU32 colour) {
	ImGui::TextUnformatted(label);
	ImGui::SameLine(ImGui::GetFontSize() * 9);
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetFontSize() * 8, h = ImGui::GetTextLineHeight() * 0.7f;
	auto* draw = ImGui::GetWindowDrawList();
	const float y = at.y + (ImGui::GetTextLineHeight() - h) / 2;
	draw->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + w, y + h), IM_COL32(0, 0, 0, 60), 2);
	draw->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + w * static_cast<float>(value) / 255, y + h), colour, 2);
	ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
	ImGui::SameLine();
	ImGui::Text("%d", value);
}

// the neurons of a lobe, from a sample's brain
std::pair<size_t, size_t> lobe_range(const Recording::Sample& s, size_t lobe) {
	size_t start = 0;
	for (size_t i = 0; i < lobe && i < s.lobe_sizes.size(); ++i) {
		start += s.lobe_sizes[i];
	}
	const size_t size = lobe < s.lobe_sizes.size() ? s.lobe_sizes[lobe] : 0;
	return {std::min(start, s.neurons.size()), std::min(start + size, s.neurons.size())};
}

constexpr size_t LOBE_DRIVE = 1;

void draw_drives(const Recording::Sample& s, int most) {
	const auto [from, to] = lobe_range(s, LOBE_DRIVE);
	std::vector<std::pair<uint8_t, size_t>> drives;
	for (size_t n = from; n < to; ++n) {
		drives.emplace_back(s.neurons[n], n - from);
	}
	if (most > 0) {
		std::stable_sort(drives.begin(), drives.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		drives.resize(std::min(drives.size(), static_cast<size_t>(most)));
	}
	for (const auto& [value, n] : drives) {
		std::string name = Kits::chemical_name(1 + n);
		level(name.empty() ? fmt::format("Drive {}", n).c_str() : name.c_str(), value, IM_COL32(220, 150, 60, 255));
	}
}

void draw_details() {
	const auto* s = selected_sample();
	if (!s_details || !s) {
		s_details = false;
		return;
	}
	ImGui::SetNextWindowPos(s_details_at, ImGuiCond_Appearing);
	ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 16, 0), ImVec2(ImGui::GetFontSize() * 26, FLT_MAX));
	if (ImGui::Begin(fmt::format("{}###recdetails", name_of(*s)).c_str(), &s_details, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::Text("%s %s, %s", s->genus == 2 ? "Grendel" : s->genus == 3 ? "Ettin" : "Norn", s->sex == 2 ? "female" : "male", Recording::life_stage_name(s->life_stage));
		ImGui::Text("Age %s", duration(s->age_ticks).c_str());
		if (s->flags & Recording::Sample::DEAD) {
			ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1), "Dead");
		} else {
			ImGui::Text("Health %d%%%s%s", s->health, s->flags & Recording::Sample::ASLEEP ? ", asleep" : "", s->flags & Recording::Sample::HELD ? ", in the hand" : "");
			if (s->gestation) {
				ImGui::Text("Pregnant (stage %d)", s->gestation);
			}
			const std::string doing = Kits::decision_name(s->decision);
			ImGui::Text("Doing: %s%s%s", doing.empty() ? "nothing" : doing.c_str(), s->doing_to.empty() ? "" : " - ", s->doing_to.c_str());
		}
		ImGui::Separator();
		ImGui::TextDisabled("Strongest drives");
		draw_drives(*s, 4);
		ImGui::Separator();
		if (ImGui::Checkbox("Keep in view", &s_follow) && s_follow) {
			g_engine_context.viewport->centre_on(s->x + s->width / 2, s->y + s->height / 2);
		}
		ImGui::SameLine();
		ImGui::Checkbox("Mark what it's looking at", &s_show_looking);
		if (ImGui::Button(s_eyes ? "Close its view" : "Creature's View")) {
			s_eyes = !s_eyes;
			s_eye_placed = false;
		}
		ImGui::SetItemTooltip("What it was looking at, as it saw it");
		ImGui::SameLine();
		if (ImGui::Button("Inspect...")) {
			s_inspector = true;
		}
	}
	ImGui::End();
}

// where the selected creature is in this frame's picture, and what it's looking at
bool box_of(uint32_t owner, Rect2f& box) {
	auto it = s_boxes.find(owner);
	if (it == s_boxes.end()) {
		return false;
	}
	box = it->second;
	return true;
}

// a world point in the window, allowing for the world wrapping round
ImVec2 to_window(float wx, float wy) {
	const auto& camera = g_engine_context.viewport->get_main_camera_rect();
	const auto& view = g_engine_context.viewport->get_screen_dest_rect();
	float dx = wx - static_cast<float>(camera.x);
	dx -= std::floor(dx / CREATURES1_WORLD_WIDTH) * CREATURES1_WORLD_WIDTH;
	if (dx > CREATURES1_WORLD_WIDTH / 2) {
		dx -= CREATURES1_WORLD_WIDTH;
	}
	return ImVec2(view.x + dx * view.width / static_cast<float>(camera.width), view.y + (wy - static_cast<float>(camera.y)) * view.height / static_cast<float>(camera.height));
}

// a ring round what the selected creature is looking at, and a line to it from its eyes
void mark_looking() {
	const auto* s = selected_sample();
	Rect2f body, it;
	if (!s_show_looking || !s || !s->attention || !box_of(s_selected, body) || !box_of(s->attention, it)) {
		return;
	}
	auto* draw = ImGui::GetBackgroundDrawList();
	const ImVec2 a = to_window(it.x - 4, it.y - 4), b = to_window(it.x + it.width + 4, it.y + it.height + 4);
	const ImVec2 eyes = to_window(body.x + body.width / 2, body.y + body.height * 0.2f);
	const ImVec2 centre((a.x + b.x) / 2, (a.y + b.y) / 2);
	const float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 3);
	const ImU32 colour = IM_COL32(255, 226, 120, static_cast<int>(200 * pulse));
	// a dotted line, from its eyes to the thing
	const float length = std::hypot(centre.x - eyes.x, centre.y - eyes.y);
	if (length < 3000) {
		for (float d = 0; d < length; d += 9) {
			const float f = d / std::max(1.f, length);
			draw->AddCircleFilled(ImVec2(eyes.x + (centre.x - eyes.x) * f, eyes.y + (centre.y - eyes.y) * f), 1.6f, colour, 6);
		}
	}
	draw->AddRect(a, b, colour, 6, 0, 2.5f);
	draw->AddText(ImVec2(a.x, b.y + 2), IM_COL32(255, 240, 200, 230), s->doing_to.c_str());
}

// the selected creature's view: what it's looking at, as Creature's View shows it
void draw_eyes() {
	const auto* s = selected_sample();
	if (!s_eyes || !s) {
		return;
	}
	const float k = UIStyle::scale();
	ImGui::SetNextWindowSize(ImVec2(EYE_WIDTH * 2.5f * k + 16, EYE_HEIGHT * 2.5f * k + ImGui::GetFrameHeight() * 2 + 12), ImGuiCond_FirstUseEver);
	if (ImGui::Begin(fmt::format("What {} sees###receyes", s->name.empty() ? s->moniker : s->name).c_str(), &s_eyes, ImGuiWindowFlags_NoSavedSettings)) {
		const ImVec2 room = ImGui::GetContentRegionAvail();
		const float scale = std::max(1.f, std::min(room.x / EYE_WIDTH, (room.y - ImGui::GetTextLineHeightWithSpacing()) / EYE_HEIGHT));
		s_eye.image(ImVec2(EYE_WIDTH * scale, EYE_HEIGHT * scale));
		ImGui::TextDisabled("%s", s->doing_to.empty() ? "Not paying attention to anything" : ("Looking at: " + s->doing_to).c_str());
	}
	ImGui::End();
}

// all of a creature as recorded: drives, chemicals and brain, as they were at this moment
void draw_inspector() {
	const auto* s = selected_sample();
	if (!s_inspector || !s) {
		return;
	}
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, ImGui::GetFontSize() * 28), ImGuiCond_FirstUseEver);
	if (ImGui::Begin(fmt::format("Inspector: {}###recinspector", name_of(*s)).c_str(), &s_inspector, ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::TextDisabled("As recorded at %s (view only)", clock_at(tick_now()).c_str());
		if (ImGui::BeginTabBar("##rectabs")) {
			if (ImGui::BeginTabItem("Drives")) {
				draw_drives(*s, 0);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Chemicals")) {
				static bool all = false;
				ImGui::Checkbox("Show those at 0 too", &all);
				if (ImGui::BeginTable("##recchems", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
					ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.5f);
					ImGui::TableSetupColumn("Chemical");
					ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableHeadersRow();
					for (size_t c = 1; c < 256; ++c) {
						if (!all && s->chemicals[c] == 0) {
							continue;
						}
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::Text("%zu", c);
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(Kits::chemical_name(c).c_str());
						ImGui::TableNextColumn();
						const ImVec2 at = ImGui::GetCursorScreenPos();
						const float w = ImGui::GetFontSize() * 6;
						ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + w * s->chemicals[c] / 255.f, at.y + ImGui::GetTextLineHeight()), IM_COL32(90, 160, 220, 140), 2);
						ImGui::Text("%d", s->chemicals[c]);
					}
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Brain")) {
				static const char* lobes[] = {"Perception", "Drive", "Source", "Verb", "Noun", "General sense", "Decision", "Attention", "Concept"};
				ImGui::TextDisabled("Each neuron's output: dark is quiet, bright is firing");
				ImGui::BeginChild("##reclobes");
				for (size_t lobe = 0; lobe < s->lobe_sizes.size(); ++lobe) {
					const auto [from, to] = lobe_range(*s, lobe);
					ImGui::Text("%s (%zu)", lobe < std::size(lobes) ? lobes[lobe] : "Lobe", to - from);
					const float cell = std::max(5.f, ImGui::GetFontSize() * 0.45f);
					const int across = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cell + 1)));
					const ImVec2 at = ImGui::GetCursorScreenPos();
					auto* draw = ImGui::GetWindowDrawList();
					for (size_t n = from; n < to; ++n) {
						const int i = static_cast<int>(n - from);
						const ImVec2 p(at.x + static_cast<float>(i % across) * (cell + 1), at.y + static_cast<float>(i / across) * (cell + 1));
						const int v = s->neurons[n];
						draw->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), IM_COL32(30 + v * 9 / 10, 30 + v * 7 / 10, 40 + v / 4, 255));
					}
					const int rows = (static_cast<int>(to - from) + across - 1) / across;
					ImGui::Dummy(ImVec2(0, static_cast<float>(rows) * (cell + 1) + 4));
				}
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Events")) {
				for (const auto& e : s_player->events()) {
					if (e.creature == s->uid && ImGui::Selectable(fmt::format("{}  {}##e{}", clock_at(e.tick), e.text, e.tick).c_str())) {
						s_position = e.tick;
						s_direction = 0;
					}
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}

void draw_events() {
	if (!s_events) {
		return;
	}
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24, ImGui::GetFontSize() * 20), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("What happened###recevents", &s_events, ImGuiWindowFlags_NoSavedSettings)) {
		static bool speech = false;
		ImGui::Checkbox("Speech too", &speech);
		ImGui::BeginChild("##reclist");
		for (const auto& e : s_player->events()) {
			if (e.kind == Recording::Event::SAID && !speech) {
				continue;
			}
			ImGui::PushStyleColor(ImGuiCol_Text, event_colour(e.kind) | IM_COL32(0, 0, 0, 255));
			if (ImGui::Selectable(fmt::format("{}  {}##e{}{}", clock_at(e.tick), e.text, e.tick, e.kind).c_str(), false)) {
				s_position = e.tick;
				s_direction = 0;
				if (e.creature) {
					s_selected = e.creature;
				}
			}
			ImGui::PopStyleColor();
		}
		ImGui::EndChild();
	}
	ImGui::End();
}

// the timeline: what happened along it, where the recording is, and dragging to scrub
void draw_timeline(float width) {
	const float h = ImGui::GetFrameHeight() * 1.1f;
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##timeline", ImVec2(width, h));
	auto* draw = ImGui::GetWindowDrawList();
	const double first = s_player->first_tick(), last = std::max<double>(s_player->last_tick(), first + 1);
	auto x_of = [&](double tick) { return at.x + static_cast<float>((tick - first) / (last - first)) * width; };
	const float mid = at.y + h / 2;
	draw->AddRectFilled(ImVec2(at.x, mid - 3), ImVec2(at.x + width, mid + 3), IM_COL32(0, 0, 0, 90), 3);
	// days and nights along it
	for (double t = first; t < last; t += 600) {
		const float daylight = std::sin(DayNight::time_of_day(static_cast<uint32_t>(t)) * 2 * 3.14159f - 3.14159f / 2) * 0.5f + 0.5f;
		draw->AddRectFilled(ImVec2(x_of(t), mid - 3), ImVec2(std::min(x_of(t + 600), at.x + width), mid + 3), IM_COL32(255, 210, 120, static_cast<int>(90 * daylight)));
	}
	draw->AddRectFilled(ImVec2(at.x, mid - 3), ImVec2(x_of(s_position), mid + 3), IM_COL32(230, 180, 90, 200), 3);
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const Recording::Event* near = nullptr;
	for (const auto& e : s_player->events()) {
		if (e.kind == Recording::Event::SAID || e.kind == Recording::Event::STARTED) {
			continue;
		}
		const float x = x_of(e.tick);
		draw->AddTriangleFilled(ImVec2(x, mid - 5), ImVec2(x - 4, mid - 11), ImVec2(x + 4, mid - 11), event_colour(e.kind));
		if (ImGui::IsItemHovered() && std::abs(mouse.x - x) < 5) {
			near = &e;
		}
	}
	const float px = x_of(s_position);
	draw->AddCircleFilled(ImVec2(px, mid), h * 0.32f, IM_COL32(250, 236, 200, 255), 16);
	draw->AddCircle(ImVec2(px, mid), h * 0.32f, IM_COL32(120, 80, 30, 255), 16, 1.5f);
	const double under = first + std::clamp(static_cast<double>(mouse.x - at.x) / width, 0.0, 1.0) * (last - first);
	if (ImGui::IsItemActive()) {
		s_position = under;
		s_scrubbing = true;
	} else {
		s_scrubbing = false;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s%s%s", clock_at(static_cast<uint32_t>(under)).c_str(), near ? "\n" : "", near ? near->text.c_str() : "");
	}
}


// --- the player's bar ---

enum Control { TO_START, STEP_BACK, BACKWARDS, PLAY, STEP_ON, TO_END, CONTROLS };

UIArt::Picture picture_for(int c) {
	switch (c) {
		case TO_START: return UIArt::Picture::ToStart;
		case STEP_BACK: return UIArt::Picture::StepBack;
		case BACKWARDS: return UIArt::Picture::Backwards;
		case PLAY: return s_direction > 0 ? UIArt::Picture::Pause : UIArt::Picture::Play;
		case STEP_ON: return UIArt::Picture::StepOn;
		default: return UIArt::Picture::ToEnd;
	}
}

const char* tip_for(int c) {
	switch (c) {
		case TO_START: return "To the start";
		case STEP_BACK: return "Back a tick (,)";
		case BACKWARDS: return s_direction < 0 ? "Stop playing backwards (K)" : "Play backwards (J)";
		case PLAY: return s_direction > 0 ? "Pause (Space)" : "Play (Space)";
		case STEP_ON: return "On a tick (.)";
		default: return "To the end";
	}
}

void press(int c, uint32_t tick) {
	switch (c) {
		case TO_START: s_position = s_player->first_tick(), s_direction = 0; break;
		case STEP_BACK: s_direction = 0, s_position = s_player->previous_tick(tick); break;
		case BACKWARDS: s_direction = s_direction < 0 ? 0 : -1; break;
		case PLAY: s_direction = s_direction > 0 ? 0 : 1; break;
		case STEP_ON: s_direction = 0, s_position = s_player->next_tick(s_player->shown_tick(tick)); break;
		default: s_position = s_player->last_tick(), s_direction = 0; break;
	}
}

std::string time_line(uint32_t tick) {
	const auto& info = s_player->info();
	const auto world = info.count("world") ? info.at("world") : std::string("A world");
	return fmt::format("{}   \xc2\xb7   {} / {}   \xc2\xb7   {}", clock_at(tick), duration(tick - s_player->first_tick()), duration(s_player->last_tick() - s_player->first_tick()), world);
}

void speed_menu() {
	if (ImGui::BeginPopup("##recspeed")) {
		for (int i = 0; i < static_cast<int>(std::size(SPEEDS)); ++i) {
			if (ImGui::Selectable(fmt::format("{}x", SPEEDS[i]).c_str(), i == s_speed)) {
				s_speed = i;
			}
		}
		ImGui::EndPopup();
	}
}

// the classic layout: a plain bar of buttons along the bottom, with the timeline under them
float classic_bar(uint32_t tick) {
	auto& io = ImGui::GetIO();
	const float k = UIStyle::scale(), pad = 8 * k;
	const float height = ImGui::GetFrameHeight() * 2 + ImGui::GetStyle().ItemSpacing.y + pad * 2;
	ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - height));
	ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, height));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
	if (ImGui::Begin("##playback", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
		const float h = ImGui::GetFrameHeight();
		for (int c = 0; c < CONTROLS; ++c) {
			ImGui::PushID(c);
			const bool on = (c == BACKWARDS && s_direction < 0) || (c == PLAY && s_direction > 0);
			if (on) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			}
			if (ImGui::Button("##c", ImVec2(h * 1.5f, h))) {
				press(c, tick);
			}
			if (on) {
				ImGui::PopStyleColor();
			}
			const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
			UIArt::controls(ImGui::GetWindowDrawList(), picture_for(c), ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), h * 0.9f, ImGui::GetColorU32(ImGuiCol_Text));
			ImGui::SetItemTooltip("%s", tip_for(c));
			ImGui::PopID();
			ImGui::SameLine();
		}
		if (ImGui::Button(fmt::format("{}x###speed", SPEEDS[s_speed]).c_str(), ImVec2(h * 2.2f, h))) {
			ImGui::OpenPopup("##recspeed");
		}
		ImGui::SetItemTooltip("Playing speed");
		speed_menu();
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(time_line(tick).c_str());
		if (s_missing > 0) {
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(0.7f, 0.3f, 0, 1), "(%d sprites aren't in this installation)", s_missing);
		}
		const auto& style = ImGui::GetStyle();
		const float buttons = ImGui::CalcTextSize("What happened").x + ImGui::CalcTextSize("Close").x + style.FramePadding.x * 4 + style.ItemSpacing.x;
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(), io.DisplaySize.x - pad - buttons));
		if (ImGui::Button("What happened")) {
			s_events = !s_events;
		}
		ImGui::SameLine();
		const bool close_now = ImGui::Button("Close");
		draw_timeline(ImGui::GetContentRegionAvail().x);
		ImGui::End();
		ImGui::PopStyleVar();
		if (close_now) {
			close();
		}
		return height;
	}
	ImGui::End();
	ImGui::PopStyleVar();
	return height;
}

// The modern layout: the game's own bar, the creature followed in the medallion on the left
// and its view in the porthole on the right, the player's controls in the middle and the
// timeline along the top of the plank.
float modern_bar(uint32_t tick) {
	auto& io = ImGui::GetIO();
	const float W = io.DisplaySize.x, H = io.DisplaySize.y;
	const float k = GameUI::modern_bar_scale();
	const float m = 8 * k, R = 62 * k, P = 72 * k;
	const float py1 = H - m, py0 = py1 - P;
	// as the game's: the medallions in the window's corners, the plank right across
	const float x0 = m, x1 = W - m;
	const ImVec2 orb_l(x0 + R + 2 * k, H - m - R - 4 * k), orb_r(x1 - R - 2 * k, H - m - R - 4 * k);
	const float inner_l = orb_l.x + R + 12 * k, inner_r = orb_r.x - R - 12 * k;
	auto* draw = ImGui::GetBackgroundDrawList();
	const auto* s = selected_sample();
	bool close_now = false;

	// the plank, as the game's, with a darker rail above it for the timeline to run along
	UIArt::wood_panel(draw, ImVec2(-4, py0 - 30 * k), ImVec2(W + 4, py0 + 4), 0, 0.5f);
	UIArt::wood_panel(draw, ImVec2(-4, py0), ImVec2(W + 4, H + 4), 0);
	draw->AddRectFilled(ImVec2(0, py0 - 2 * k), ImVec2(W, py0 + 3 * k), UIArt::BRASS_DARK);
	draw->AddLine(ImVec2(0, py0 - 1 * k), ImVec2(W, py0 - 1 * k), UIArt::BRASS_LIGHT, std::max(1.f, 1.2f * k));
	for (float x : {inner_l - 2 * k, inner_r + 2 * k}) {
		UIArt::rivet(draw, ImVec2(x, py0 + P * 0.3f), 3.2f * k);
		UIArt::rivet(draw, ImVec2(x, py0 + P * 0.75f), 3.2f * k);
	}
	// the controls, in the raised cartouche at the centre
	const float r = 19 * k, big = 26 * k, gap = 9 * k;
	const float controls_w = 5 * 2 * r + 2 * big + 6 * gap;
	const float mid = (inner_l + inner_r) / 2, cy = (py0 + py1) / 2;
	const ImVec2 ca(mid - controls_w / 2 - 14 * k, py0 - 6 * k), cb(mid + controls_w / 2 + 14 * k, py1 + 6 * k);
	UIArt::wood_panel(draw, ca, cb, (cb.y - ca.y) / 2, 0.25f);
	draw->AddRect(ca, cb, UIArt::BRASS, (cb.y - ca.y) / 2, 0, 2.5f * k);
	draw->AddRect(ImVec2(ca.x + 3 * k, ca.y + 3 * k), ImVec2(cb.x - 3 * k, cb.y - 3 * k), IM_COL32(255, 226, 160, 60), (cb.y - ca.y) / 2 - 3 * k, 0, 1);
	UIArt::leaf(draw, ImVec2(orb_l.x + R * 0.55f, orb_l.y - R * 0.75f), 26 * k, -0.4f, IM_COL32(86, 140, 58, 255));
	UIArt::leaf(draw, ImVec2(orb_l.x + R * 0.7f, orb_l.y - R * 0.6f), 20 * k, 0.15f, IM_COL32(110, 168, 70, 255));
	UIArt::leaf(draw, ImVec2(orb_r.x - R * 0.55f, orb_r.y - R * 0.75f), 26 * k, 3.14159f + 0.4f, IM_COL32(86, 140, 58, 255));
	UIArt::leaf(draw, ImVec2(orb_r.x - R * 0.7f, orb_r.y - R * 0.6f), 20 * k, 3.14159f - 0.15f, IM_COL32(110, 168, 70, 255));

	if (GameUI::begin_bar_area("##recplank", ImVec2(inner_l - 4 * k, py0 - 26 * k), ImVec2(inner_r + 4 * k, py1 + 6 * k))) {
		float x = mid - controls_w / 2;
		for (int c = 0; c < CONTROLS; ++c) {
			const float radius = (c == PLAY || c == BACKWARDS) ? big : r;
			const bool on = (c == BACKWARDS && s_direction < 0) || (c == PLAY && s_direction > 0);
			if (UIArt::round_button(draw, fmt::format("##rc{}", c).c_str(), ImVec2(x + radius, cy), radius, picture_for(c), tip_for(c), on)) {
				press(c, tick);
			}
			x += 2 * radius + gap;
		}
		// the name and what it's doing, carved into the left of the plank
		const float size = ImGui::GetFontSize() * std::max(1.f, k / UIStyle::scale());
		const float name_w = ca.x - gap - inner_l;
		if (name_w > 60 * k) {
			if (s) {
				const std::string name = s->name.empty() || s->name == "<Un Named>" ? s->moniker : s->name;
				UIArt::text(draw, ImVec2(inner_l + 6 * k, cy - size - 2 * k), size * 1.15f, UIArt::INK, name.c_str());
				if (name_w > 140 * k) {
					const std::string doing = Kits::decision_name(s->decision);
					const std::string line = fmt::format("{} \xc2\xb7 {}%{}{}", (s->flags & Recording::Sample::DEAD) ? "Dead" : (s->flags & Recording::Sample::ASLEEP) ? "Asleep" : Recording::life_stage_name(s->life_stage), s->health, doing.empty() ? "" : " \xc2\xb7 ", doing);
					UIArt::text(draw, ImVec2(inner_l + 6 * k, cy + 3 * k), size * 0.92f, UIArt::INK_SOFT, line.c_str());
				}
				ImGui::SetCursorScreenPos(ImVec2(inner_l, py0 + 6 * k));
				if (ImGui::InvisibleButton("##recname", ImVec2(name_w, P - 12 * k))) {
					s_details = !s_details;
					s_details_at = ImVec2(inner_l, py0 - 300 * k);
				}
				ImGui::SetItemTooltip("Click for what it's doing, and its inspector");
			} else {
				UIArt::text(draw, ImVec2(inner_l + 6 * k, cy - size / 2), size, UIArt::INK_SOFT, "Click a creature to follow it");
			}
		}
		// to the right: the speed, keeping it in view, what happened, closing
		float sx = cb.x + gap + r;
		ImGui::SetCursorScreenPos(ImVec2(sx - r, cy - r));
		if (ImGui::InvisibleButton("##recspeedplate", ImVec2(2 * r, 2 * r))) {
			ImGui::OpenPopup("##recspeed");
		}
		ImGui::SetItemTooltip("Playing speed");
		UIArt::brass_ring(draw, ImVec2(sx, cy), r, r * 0.78f);
		draw->AddCircleFilled(ImVec2(sx, cy), r * 0.78f, IM_COL32(60, 40, 24, 255), 24);
		const std::string speed = fmt::format("{}x", SPEEDS[s_speed]);
		UIArt::text(draw, ImVec2(sx - ImGui::CalcTextSize(speed.c_str()).x * 0.5f, cy - ImGui::GetFontSize() * 0.5f), ImGui::GetFontSize(), IM_COL32(250, 230, 190, 255), speed.c_str());
		speed_menu();
		const float stud = 2 * r + 8 * k;
		sx += stud;
		if (sx + r < inner_r && UIArt::round_button(draw, "##recfollow", ImVec2(sx, cy), r, UIArt::Picture::Follow, "Keep it in view", s_follow)) {
			s_follow = !s_follow;
		}
		sx += stud;
		if (sx + r < inner_r && UIArt::round_button(draw, "##recevents", ImVec2(sx, cy), r, UIArt::Picture::Records, "What happened", s_events)) {
			s_events = !s_events;
		}
		sx += stud;
		if (sx + r < inner_r && UIArt::round_button(draw, "##recinspect", ImVec2(sx, cy), r, UIArt::Picture::Lab, "Its inspector: drives, chemicals and brain", s_inspector, s != nullptr)) {
			s_inspector = !s_inspector;
		}
		sx += stud;
		if (UIArt::round_button(draw, "##recclose", ImVec2(std::min(sx, inner_r - r), cy), r, UIArt::Picture::Quit, "Close the recording")) {
			close_now = true;
		}
		// the time and the timeline, along the top of the plank
		const std::string when = time_line(tick);
		UIArt::text(draw, ImVec2(inner_l + 6 * k, py0 - 26 * k - ImGui::GetFontSize() * 1.2f), ImGui::GetFontSize(), IM_COL32(250, 238, 214, 255), when.c_str());
		if (s_missing > 0) {
			const std::string missing = fmt::format("{} sprites aren't in this installation", s_missing);
			UIArt::text(draw, ImVec2(inner_r - ImGui::CalcTextSize(missing.c_str()).x - 6 * k, py0 - 26 * k - ImGui::GetFontSize() * 1.2f), ImGui::GetFontSize(), IM_COL32(255, 200, 120, 255), missing.c_str());
		}
		ImGui::SetCursorScreenPos(ImVec2(inner_l, py0 - 24 * k));
		draw_timeline(inner_r - inner_l);
	}
	ImGui::End();

	// the medallion: the creature followed, as it was
	if (GameUI::begin_bar_area("##recportrait", ImVec2(orb_l.x - R, orb_l.y - R), ImVec2(orb_l.x + R, orb_l.y + R))) {
		ImGui::SetCursorScreenPos(ImVec2(orb_l.x - R, orb_l.y - R));
		if (ImGui::InvisibleButton("##recportraitbutton", ImVec2(2 * R, 2 * R)) && s) {
			s_details = !s_details;
			s_details_at = ImVec2(orb_l.x, orb_l.y - 300 * k);
		}
		ImGui::SetItemTooltip("%s", s ? "What it's doing" : "Click a creature in the world to follow it");
	}
	ImGui::End();
	UIArt::shadow_circle(draw, orb_l, R);
	UIArt::brass_ring(draw, orb_l, R, R * 0.84f);
	UIArt::glass(draw, orb_l, R * 0.84f);
	const float pr = R * 0.84f;
	if (!(s && s_face.draw(draw, ImVec2(orb_l.x - pr, orb_l.y - pr), ImVec2(orb_l.x + pr, orb_l.y + pr), pr))) {
		UIArt::picture(draw, UIArt::Picture::Egg, orb_l, R * 0.9f);
	}
	if (s) {
		const float health = (s->flags & Recording::Sample::DEAD) ? 0 : static_cast<float>(s->health) / 100.f;
		UIArt::ring_meter(draw, orb_l, R * 0.77f, 5 * k, health, ImGui::ColorConvertFloat4ToU32(UIStyle::health_colour(health)));
	}
	UIArt::glass_shine(draw, orb_l, pr);

	// the porthole: what it's looking at
	if (GameUI::begin_bar_area("##recporthole", ImVec2(orb_r.x - R, orb_r.y - R), ImVec2(orb_r.x + R, orb_r.y + R))) {
		ImGui::SetCursorScreenPos(ImVec2(orb_r.x - R, orb_r.y - R));
		if (ImGui::InvisibleButton("##recportholebutton", ImVec2(2 * R, 2 * R)) && s) {
			s_eyes = !s_eyes;
		}
		if (s) {
			ImGui::SetItemTooltip("Creature's View: %s\nClick for a bigger view", s->doing_to.empty() ? "not paying attention to anything" : ("looking at " + s->doing_to).c_str());
		}
	}
	ImGui::End();
	UIArt::shadow_circle(draw, orb_r, R);
	UIArt::brass_ring(draw, orb_r, R, R * 0.84f);
	for (int i = 0; i < 8; ++i) {
		const float a = static_cast<float>(i) * 3.14159265f / 4 + 0.39f;
		UIArt::rivet(draw, ImVec2(orb_r.x + std::cos(a) * R * 0.92f, orb_r.y + std::sin(a) * R * 0.92f), 2.6f * k);
	}
	UIArt::glass(draw, orb_r, pr);
	if (!(s && s_eye_placed && s_eye.draw(draw, ImVec2(orb_r.x - pr, orb_r.y - pr), ImVec2(orb_r.x + pr, orb_r.y + pr), pr))) {
		UIArt::picture(draw, UIArt::Picture::Follow, orb_r, R * 0.7f);
	}
	UIArt::glass_shine(draw, orb_r, pr);
	if (close_now) {
		close();
	}
	return P + m + 30 * k;
}

} // namespace

bool playing_back() {
	return s_player != nullptr;
}

fs::path folder() {
	return current_world_folder() / "Recordings";
}

void open(const fs::path& file) {
	try {
		auto player = std::make_unique<Recording::Player>(file);
		close();
		s_player = std::move(player);
		s_position = s_player->first_tick();
		s_direction = 0;
		s_selected = 0;
		s_details = s_inspector = s_follow = s_eyes = false;
		s_missing = 0;
		DayNight::show_playback(&s_shown);
		for (const auto& c : s_player->creatures_at(s_player->first_tick())) {
			if (!(c.flags & Recording::Sample::DEAD)) {
				s_selected = c.uid;
				break;
			}
		}
		fmt::print("* Playing back {} (ticks {} to {})\n", file.string(), s_player->first_tick(), s_player->last_tick());
	} catch (const std::exception& e) {
		GameUI::show_message(fmt::format("Couldn't play {}: {}", file.filename().string(), e.what()));
	}
}

void close() {
	s_player.reset();
	s_galleries.clear();
	DayNight::show_playback(nullptr);
}

void toggle_recording() {
	if (Recording::recording()) {
		Recording::stop();
		return;
	}
	char when[64] = {};
	const std::time_t now = std::time(nullptr);
	std::strftime(when, sizeof(when), "%Y-%m-%d %H.%M.%S", std::localtime(&now));
	const auto path = folder() / (fs::path(get_world_file_name()).stem().string() + " " + when + Recording::EXTENSION);
	try {
		Recording::start(path);
	} catch (const std::exception& e) {
		GameUI::show_message(fmt::format("Couldn't start recording: {}", e.what()));
	}
}

void menu_items() {
	if (ImGui::MenuItem(Recording::recording() ? "Stop recording" : "Record this world", nullptr, Recording::recording(), !playing_back())) {
		toggle_recording();
	}
	ImGui::SetItemTooltip("Records the world as it runs, to watch again later: forwards, backwards and at any speed");
	if (ImGui::MenuItem("Play a recording...")) {
		FileDialogs::open("Play a recording", {{"Recordings (*.c1rec)", "c1rec"}}, folder().string(), false, [](const std::vector<std::string>& paths) {
			if (!paths.empty()) {
				open(paths.front());
			}
		});
	}
	std::error_code err;
	std::vector<fs::path> recent;
	for (const auto& entry : fs::directory_iterator(folder(), err)) {
		if (entry.path().extension() == Recording::EXTENSION && entry.path() != Recording::file()) {
			recent.push_back(entry.path());
		}
	}
	std::sort(recent.begin(), recent.end(), [](const fs::path& a, const fs::path& b) { return a.filename() > b.filename(); });
	if (!recent.empty() && ImGui::BeginMenu("Recent recordings")) {
		for (size_t i = 0; i < recent.size() && i < 10; ++i) {
			if (ImGui::MenuItem(recent[i].stem().string().c_str())) {
				open(recent[i]);
			}
		}
		ImGui::EndMenu();
	}
}

void draw_recording_light() {
	if (!Recording::recording() || playing_back()) {
		return;
	}
	const auto& view = g_engine_context.viewport->get_screen_dest_rect();
	const std::string text = fmt::format("REC  {}  {:.1f} MB", duration(Recording::ticks_recorded()), static_cast<double>(Recording::bytes_written()) / 1e6);
	const ImVec2 size = ImGui::CalcTextSize(text.c_str());
	const float k = UIStyle::scale(), pad = 8 * k, dot = 5 * k;
	const ImVec2 pos(view.x + view.width / 2 - (size.x + dot * 3 + pad * 2) / 2, view.y + 8 * k);
	auto* draw = ImGui::GetForegroundDrawList();
	const ImVec2 end(pos.x + size.x + dot * 3 + pad * 2, pos.y + size.y + pad);
	draw->AddRectFilled(pos, end, IM_COL32(30, 20, 14, 190), (end.y - pos.y) / 2);
	const float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 3);
	draw->AddCircleFilled(ImVec2(pos.x + pad + dot, (pos.y + end.y) / 2), dot, IM_COL32(230, 50, 40, static_cast<int>(255 * pulse)), 12);
	draw->AddText(ImVec2(pos.x + pad + dot * 3, pos.y + pad / 2), IM_COL32(245, 230, 210, 255), text.c_str());
}

float draw(float top) {
	if (!s_player) {
		return 0;
	}
	auto& io = ImGui::GetIO();
	// playing
	if (s_direction && !s_scrubbing) {
		s_position += s_direction * SPEEDS[s_speed] * TICKS_PER_SECOND * io.DeltaTime;
		if (s_position >= s_player->last_tick() || s_position <= s_player->first_tick()) {
			s_position = std::clamp(s_position, static_cast<double>(s_player->first_tick()), static_cast<double>(s_player->last_tick()));
			s_direction = 0;
		}
	}
	const uint32_t tick = tick_now();
	// keys: space plays or pauses, J K L backwards, pause, forwards, comma and full stop step
	if (!io.WantTextInput) {
		if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
			s_direction = s_direction ? 0 : 1;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_J, false)) {
			s_direction = -1;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_K, false)) {
			s_direction = 0;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {
			s_direction = 1;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Comma)) {
			s_direction = 0;
			s_position = s_player->previous_tick(tick);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Period)) {
			s_direction = 0;
			s_position = s_player->next_tick(s_player->shown_tick(tick));
		}
	}
	// clicking a creature in the view shows what it was doing
	if (!io.WantCaptureMouse && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
		const float wx = g_engine_context.viewport->window_x_to_world_x(io.MousePos.x), wy = g_engine_context.viewport->window_y_to_world_y(io.MousePos.y);
		for (const auto& s : s_player->creatures_at(tick)) {
			float dx = wx - s.x;
			dx -= std::floor(dx / CREATURES1_WORLD_WIDTH) * CREATURES1_WORLD_WIDTH;
			if (dx <= s.width && wy >= s.y && wy <= s.y + s.height) {
				s_selected = s.uid;
				s_details = true;
				s_details_at = ImVec2(io.MousePos.x + 16, io.MousePos.y - 40);
				break;
			}
		}
	}
	if (s_follow) {
		Rect2f body;
		if (box_of(s_selected, body)) {
			g_engine_context.viewport->centre_on(body.x + body.width / 2, body.y + body.height / 2);
		} else if (const auto* s = selected_sample()) {
			g_engine_context.viewport->centre_on(s->x + s->width / 2, s->y + s->height / 2);
		}
	}

	const float kept = UIStyle::classic() ? classic_bar(tick) : modern_bar(tick);
	if (!s_player) {
		return 0; // closed from the bar
	}
	mark_looking();
	draw_details();
	draw_eyes();
	draw_inspector();
	draw_events();
	(void)top;
	return kept;
}

void render(RenderSystem& system) {
	if (!s_player) {
		return;
	}
	const uint32_t tick = s_player->shown_tick(tick_now());
	// the background: the world's own, with its higher-resolution tiles where it has them,
	// or the recording's
	if (const auto* back = s_player->background()) {
		const auto& g = gallery_for(back->id);
		if (HdArt::enabled() && g.size() > 0 && g_engine_context.map->get_background().width(0) == g.width(0)) {
			g_engine_context.map->render(system);
		} else if (g.size() > 0) {
			system.add(create_render_item_texture(g.texture, g.texture_locations[0], 0, 0, 0), LAYER_BACKGROUND);
		}
	}
	s_shown = DayNight::Shown{};
	s_shown.tick = tick;
	s_boxes.clear();
	s_player->items_between(s_position, s_items);
	for (const auto& item : s_items) {
		const auto& g = gallery_for(item.asset);
		if (!g.has_frame(item.frame)) {
			continue;
		}
		system.add(HdArt::render_item(g, item.frame, item.x, item.y, item.z));
		const Rect2f box{item.x, item.y, static_cast<float>(g.width(item.frame)), static_cast<float>(g.height(item.frame))};
		auto [it, added] = s_boxes.emplace(item.owner, box);
		if (!added) {
			const float x0 = std::min(it->second.x, box.x), y0 = std::min(it->second.y, box.y);
			const float x1 = std::max(it->second.x + it->second.width, box.x + box.width), y1 = std::max(it->second.y + it->second.height, box.y + box.height);
			it->second = Rect2f{x0, y0, x1 - x0, y1 - y0};
		}
	}
	for (const auto& [uid, box] : s_boxes) {
		const auto* owner = s_player->owner(uid);
		if (owner && owner->kind == Recording::Owner::EGG) {
			s_shown.eggs.push_back(box);
			s_shown.egg_seeds.push_back(uid & 0xffff);
		} else if (owner && owner->kind == Recording::Owner::HAND) {
			s_shown.hands.push_back(box);
		}
	}
	// the selected creature's view, centred on what it's looking at (or on itself), moving
	// smoothly as its attention moves
	// and its face, for the medallion
	if (Rect2f body; s_selected && box_of(s_selected, body)) {
		constexpr int FACE = 56;
		const int32_t left = static_cast<int32_t>(body.x + body.width / 2) - FACE / 2;
		const int32_t top = std::clamp(static_cast<int32_t>(body.y + body.height * 0.3f) - FACE / 2, 0, CREATURES1_WORLD_HEIGHT - FACE);
		s_face.look_at(Rect2i{left, top, FACE, FACE}, FACE, FACE);
		s_face.render(system);
	}
	if (s_eyes || !UIStyle::classic()) {
		const auto* s = selected_sample();
		Rect2f at;
		if (s && ((s->attention && box_of(s->attention, at)) || box_of(s_selected, at))) {
			const float tx = at.x + at.width / 2, ty = at.y + at.height / 2;
			float dx = tx - s_eye_x;
			dx -= std::round(dx / CREATURES1_WORLD_WIDTH) * CREATURES1_WORLD_WIDTH;
			const float follow = std::min(1.f, ImGui::GetIO().DeltaTime * 8);
			if (!s_eye_placed || std::abs(dx) > 640 || std::abs(ty - s_eye_y) > 480) {
				s_eye_x = tx, s_eye_y = ty;
				s_eye_placed = true;
			} else {
				s_eye_x += dx * follow;
				s_eye_y += (ty - s_eye_y) * follow;
			}
			const int32_t left = static_cast<int32_t>(s_eye_x) - EYE_WIDTH / 2;
			const int32_t top = std::clamp(static_cast<int32_t>(s_eye_y) - EYE_HEIGHT / 2, 0, CREATURES1_WORLD_HEIGHT - EYE_HEIGHT);
			s_eye.look_at(Rect2i{left, top, EYE_WIDTH, EYE_HEIGHT}, EYE_WIDTH, EYE_HEIGHT);
			s_eye.render(system);
		}
	}
}

void testing() {
	// debugging: OPENCREATURES1_TEST_PLAY=<file> opens a recording and steps through it both
	// ways, printing what it finds
	if (static const char* play = std::getenv("OPENCREATURES1_TEST_PLAY"); play) {
		static bool done = false;
		if (!done && ImGui::GetTime() > 1) {
			done = true;
			open(play);
			if (s_player) {
				size_t forwards = 1, backwards = 1, empty = 0;
				for (uint32_t t = s_player->first_tick(); t < s_player->last_tick(); t = s_player->next_tick(t)) {
					forwards++;
					empty += s_player->items_at(t).empty();
				}
				for (uint32_t t = s_player->last_tick(); t > s_player->first_tick(); t = s_player->previous_tick(t)) {
					backwards++;
				}
				fmt::print("TEST PLAY: ticks {}-{}, {} forwards, {} backwards, {} empty, {} events, {} creatures at the end\n", s_player->first_tick(),
					s_player->last_tick(), forwards, backwards, empty, s_player->events().size(), s_player->creatures_at(s_player->last_tick()).size());
				s_position = s_player->last_tick();
				s_direction = -1;
				s_speed = 5;
			}
		}
		return;
	}
	static const char* test = std::getenv("OPENCREATURES1_TEST_RECORDING");
	static int stage = 0;
	if (!test || stage == 3) {
		return;
	}
	float seconds = 10, fraction = 0.5f;
	std::sscanf(test, "%f,%f", &seconds, &fraction);
	const double now = ImGui::GetTime();
	if (stage == 0 && now > 1) {
		toggle_recording();
		stage = 1;
	} else if (stage == 1 && now > 1 + seconds) {
		const auto path = Recording::file();
		Recording::stop();
		open(path);
		stage = 2;
		if (s_player) {
			s_position = s_player->first_tick() + fraction * (s_player->last_tick() - s_player->first_tick());
			const auto named = std::count_if(s_player->assets().begin(), s_player->assets().end(), [](const Recording::Asset& x) { return x.from_game; });
			fmt::print("TEST RECORDING: {} bytes, ticks {}-{}, {} sprites ({} the game's own, by name), {} events, {} creatures at the middle\n", fs::file_size(path),
				s_player->first_tick(), s_player->last_tick(), s_player->assets().size(), named, s_player->events().size(), s_player->creatures_at(tick_now()).size());
			for (const auto& x : s_player->assets()) {
				if (!x.from_game) {
					fmt::print("TEST RECORDING: kept the pictures of {:?} ({} images)\n", x.name, x.images.size());
				}
			}
		}
	} else if (stage == 2 && s_player) {
		for (const auto& c : s_player->creatures_at(tick_now())) {
			if (!(c.flags & Recording::Sample::DEAD)) {
				s_selected = c.uid;
				s_details = true;
				s_inspector = std::getenv("OPENCREATURES1_TEST_INSPECTOR") != nullptr;
				s_eyes = std::getenv("OPENCREATURES1_TEST_EYES") != nullptr;
				s_details_at = ImVec2(40, 120);
				s_follow = true;
				break;
			}
		}
		stage = 3;
	}
}

} // namespace RecordingUI
