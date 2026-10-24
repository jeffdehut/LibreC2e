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

const ImageGallery& gallery_for(uint32_t asset) {
	static const ImageGallery none;
	auto it = s_galleries.find(asset);
	if (it != s_galleries.end()) {
		return it->second;
	}
	const auto& assets = s_player->assets();
	if (asset >= assets.size() || assets[asset].images.empty()) {
		return none;
	}
	const auto& a = assets[asset];
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
		if (ImGui::Button("Inspect...")) {
			s_inspector = true;
		}
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
		s_details = s_inspector = s_follow = false;
		DayNight::show_playback(&s_shown);
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
		if (const auto* s = selected_sample()) {
			g_engine_context.viewport->centre_on(s->x + s->width / 2, s->y + s->height / 2);
		}
	}

	// the bar
	const float k = UIStyle::scale(), pad = 10 * k;
	const ImVec2 display = io.DisplaySize;
	const float height = ImGui::GetFrameHeight() * 2 + ImGui::GetStyle().ItemSpacing.y + pad * 2;
	const ImVec2 pos(12 * k, display.y - height - 10 * k);
	const ImVec2 size(display.x - 24 * k, height);
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	const bool classic = UIStyle::classic();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
	if (!classic) {
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(250, 236, 210, 255));
	}
	if (ImGui::Begin("##playback", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | (classic ? 0 : ImGuiWindowFlags_NoBackground))) {
		if (!classic) {
			auto* draw = ImGui::GetWindowDrawList();
			UIArt::wood_panel(draw, pos, ImVec2(pos.x + size.x, pos.y + size.y), 14 * k, 0.15f);
			draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), UIArt::BRASS, 14 * k, 0, 2);
		}
		auto button = [](const char* label, const char* tip) {
			const bool pressed = ImGui::Button(label, ImVec2(ImGui::GetFrameHeight() * 1.6f, 0));
			ImGui::SetItemTooltip("%s", tip);
			ImGui::SameLine();
			return pressed;
		};
		if (button("|<", "To the start")) {
			s_position = s_player->first_tick();
			s_direction = 0;
		}
		if (button("<|", "Back a tick (,)")) {
			s_direction = 0;
			s_position = s_player->previous_tick(tick);
		}
		if (button(s_direction < 0 ? "<< *" : "<<", "Play backwards (J)")) {
			s_direction = s_direction < 0 ? 0 : -1;
		}
		if (button(s_direction > 0 ? "||" : ">", s_direction > 0 ? "Pause (Space)" : "Play (Space)")) {
			s_direction = s_direction > 0 ? 0 : 1;
		}
		if (button("|>", "On a tick (.)")) {
			s_direction = 0;
			s_position = s_player->next_tick(s_player->shown_tick(tick));
		}
		if (button(">|", "To the end")) {
			s_position = s_player->last_tick();
			s_direction = 0;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
		if (ImGui::BeginCombo("##speed", fmt::format("{}x", SPEEDS[s_speed]).c_str())) {
			for (int i = 0; i < static_cast<int>(std::size(SPEEDS)); ++i) {
				if (ImGui::Selectable(fmt::format("{}x", SPEEDS[i]).c_str(), i == s_speed)) {
					s_speed = i;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SetItemTooltip("Playing speed");
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		const auto& info = s_player->info();
		const auto world = info.count("world") ? info.at("world") : std::string("A world");
		ImGui::Text("%s   %s / %s   %s", clock_at(tick).c_str(), duration(tick - s_player->first_tick()).c_str(),
			duration(s_player->last_tick() - s_player->first_tick()).c_str(), world.c_str());
		const float right = ImGui::GetFontSize() * 11;
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(), size.x - pad - right));
		if (ImGui::Button("What happened")) {
			s_events = !s_events;
		}
		ImGui::SameLine();
		if (ImGui::Button("Close")) {
			close();
			ImGui::End();
			ImGui::PopStyleVar();
			if (!classic) {
				ImGui::PopStyleColor();
			}
			return 0;
		}
		draw_timeline(ImGui::GetContentRegionAvail().x);
	}
	ImGui::End();
	ImGui::PopStyleVar();
	if (!classic) {
		ImGui::PopStyleColor();
	}
	draw_details();
	draw_inspector();
	draw_events();
	(void)top;
	return height + 20 * k;
}

void render(RenderSystem& system) {
	if (!s_player) {
		return;
	}
	const uint32_t tick = s_player->shown_tick(tick_now());
	// the background: the world's own, with its higher-resolution tiles where it has them,
	// or the recording's
	const auto* back = s_player->background();
	if (HdArt::enabled() && back && g_engine_context.map->get_background().width(0) == back->images.front().width) {
		g_engine_context.map->render(system);
	} else if (back) {
		const auto& g = gallery_for(back->id);
		if (g.size() > 0) {
			system.add(create_render_item_texture(g.texture, g.texture_locations[0], 0, 0, 0), LAYER_BACKGROUND);
		}
	}
	s_shown = DayNight::Shown{};
	s_shown.tick = tick;
	std::map<uint32_t, Rect2f> glowing;
	for (const auto& item : s_player->items_at(tick)) {
		const auto& g = gallery_for(item.asset);
		if (!g.has_frame(item.frame)) {
			continue;
		}
		system.add(HdArt::render_item(g, item.frame, item.x, item.y, item.z));
		if (const auto* owner = s_player->owner(item.owner); owner && (owner->kind == Recording::Owner::EGG || owner->kind == Recording::Owner::HAND)) {
			const Rect2f box{item.x, item.y, static_cast<float>(g.width(item.frame)), static_cast<float>(g.height(item.frame))};
			auto [it, added] = glowing.emplace(item.owner, box);
			if (!added) {
				const float x0 = std::min(it->second.x, box.x), y0 = std::min(it->second.y, box.y);
				const float x1 = std::max(it->second.x + it->second.width, box.x + box.width), y1 = std::max(it->second.y + it->second.height, box.y + box.height);
				it->second = Rect2f{x0, y0, x1 - x0, y1 - y0};
			}
		}
	}
	for (const auto& [uid, box] : glowing) {
		if (s_player->owner(uid)->kind == Recording::Owner::EGG) {
			s_shown.eggs.push_back(box);
			s_shown.egg_seeds.push_back(uid & 0xffff);
		} else {
			s_shown.hands.push_back(box);
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
			fmt::print("TEST RECORDING: {} bytes, ticks {}-{}, {} sprites, {} events, {} creatures at the middle\n", fs::file_size(path),
				s_player->first_tick(), s_player->last_tick(), s_player->assets().size(), s_player->events().size(), s_player->creatures_at(tick_now()).size());
		}
	} else if (stage == 2 && s_player) {
		for (const auto& c : s_player->creatures_at(tick_now())) {
			if (!(c.flags & Recording::Sample::DEAD)) {
				s_selected = c.uid;
				s_details = true;
				s_inspector = std::getenv("OPENCREATURES1_TEST_INSPECTOR") != nullptr;
				s_details_at = ImVec2(40, 120);
				s_follow = true;
				break;
			}
		}
		stage = 3;
	}
}

} // namespace RecordingUI
