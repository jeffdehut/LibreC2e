#include "common/SessionProfile.h"
#include "Kits.h"
#include "GameText.h"

#include "GameUI.h"
#include "KitStock.h"
#include "KitPreferences.h"
#include "KitMonitoring.h"
#include "Graveyard.h"
#include "MapManager.h"
#include "Registration.h"
#include "PersistentFile.h"
#include <sstream>
#include "PhotoAlbum.h"
#include "UIImages.h"
#include "UIArt.h"
#include "UIStyle.h"
#include "KitWindows.h"
#include "Wolfling.h"
#include "Family.h"
#include <ctime>
#include <cstdlib>

#include "C1SoundManager.h"
#include "CobInjector.h"
#include "Macro.h"
#include "MacroManager.h"
#include "CreatureBrain.h"
#include "CreatureGenetics.h"
#include "EngineContext.h"
#include "ImageManager.h"
#include "MessageManager.h"
#include "PathManager.h"
#include "PointerManager.h"
#include "ViewportManager.h"
#include "WorldState.h"
#include "WorldSystems.h"
#include "common/Ascii.h"
#include "common/Exception.h"
#include "common/Random.h"
#include "fileformats/c1cobfile.h"
#include "fileformats/genomeFile.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "fileformats/sfc/CBrain.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"
#include "objects/ObjectNames.h"
#include "objects/SimpleObject.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <map>
#include <optional>
#include <array>
#include <deque>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <cfloat>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Kits {

namespace {

enum Kit {
	KIT_OWNERS,
	KIT_HEALTH,
	KIT_SCIENCE,
	KIT_BREEDERS,
	KIT_HATCHERY,
	KIT_INJECTOR,
	KIT_FUNERAL,
	KIT_SCORE,
	KIT_OBSERVATION,
	KIT_BACKUP,
	KIT_COUNT
};

struct KitInfo {
	const char* name;
	const char* button;
};

constexpr std::array<KitInfo, KIT_COUNT> KITS = {{
	{"Owner's Kit", "Owner"},
	{"Health Kit", "Health"},
	{"Science Kit", "Science"},
	{"Breeder's Kit", "Breeder"},
	{"The Hatchery", "Hatchery"},
	{"Injector Kit", "Injector"},
	{"Funeral Kit", "Funeral"},
	{"Score Kit", "Score"},
	{"Observation Kit", "Observe"},
	{"Backup and Emergency", "Backup"},
}};

std::array<bool, KIT_COUNT> s_open{};
std::array<bool, KIT_COUNT> s_restore_page{};
KitPreferences s_preferences;
fs::path s_preferences_path;
size_t s_current_kit = 0;
int s_check_frame = 0;
int s_page_index = 0;
std::set<std::string> s_checked_pages;
std::string s_kit_error;
bool s_tabbed = false;
float s_panel_top = 0, s_panel_bottom = 0;

void save_preference(const std::string& key, int value) {
    try { s_preferences.set(key, value); }
    catch (const std::exception& e) { s_kit_error = e.what(); }
}

int page_id(const char* text) {
    uint32_t hash = 2166136261u;
    while (*text) { hash = (hash ^ static_cast<uint8_t>(*text++)) * 16777619u; }
    return static_cast<int>(hash & 0x7fffffffu);
}

bool begin_page(const char* label) {
    const std::string key = "page." + std::string(KITS[s_current_kit].button);
    const int saved = s_preferences.get(key, 0);
    const int page = s_page_index++;
    const bool checking = std::getenv("OPENC1_KIT_CHECK") && !session_profile().empty();
    const bool restore = checking ? (s_check_frame / 4) % 7 == page : s_restore_page[s_current_kit] && saved == page_id(label);
    const bool active = ImGui::BeginTabItem(label, nullptr, restore ? ImGuiTabItemFlags_SetSelected : 0);
    if (active && checking && s_checked_pages.insert(std::string(KITS[s_current_kit].button) + ":" + label).second)
        fmt::print("TEST KIT PAGE: {} / {}\n", KITS[s_current_kit].button, label);
    if (active && (restore || saved == 0 || !s_restore_page[s_current_kit])) {
        s_restore_page[s_current_kit] = false;
        save_preference(key, page_id(label));
    }
    return active;
}

// --- game data the kits use ---

// .str files: Pascal strings, some with a 16-bit count first
std::vector<std::string> read_strings(const std::string& file, bool counted) {
	std::vector<std::string> result;
	auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, file);
	if (path.empty()) {
		return result;
	}
	std::ifstream in(path, std::ios::binary);
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	size_t i = counted ? 2 : 0;
	while (i < data.size()) {
		size_t len = data[i++];
		result.emplace_back(data.begin() + static_cast<long>(i), data.begin() + static_cast<long>(std::min(data.size(), i + len)));
		i += len;
	}
	return result;
}

struct Theme {
	std::string name;
	std::array<uint8_t, 4> chemicals{};
};

// themes.str: a count, then for each chemical group its name and four chemical numbers
std::vector<Theme> read_themes() {
	std::vector<Theme> result;
	auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "themes.str");
	if (path.empty()) {
		return result;
	}
	std::ifstream in(path, std::ios::binary);
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	size_t i = 2;
	while (i < data.size()) {
		Theme t;
		size_t len = data[i++];
		t.name.assign(data.begin() + static_cast<long>(i), data.begin() + static_cast<long>(std::min(data.size(), i + len)));
		i += len;
		for (auto& c : t.chemicals) {
			c = i < data.size() ? data[i++] : 0;
		}
		result.push_back(t);
	}
	return result;
}

const std::vector<std::string>& chemical_names() {
	static std::vector<std::string> names;
    static uint64_t generation = 0;
	if (names.empty() || generation != g_engine_context.generation()) {
        generation = g_engine_context.generation();
		names = read_strings("chemicals.str", false);
		names.resize(256);
		for (size_t i = 0; i < names.size(); ++i) {
			if (names[i].empty()) {
				names[i] = fmt::format("Chemical {}", i);
			}
		}
	}
	return names;
}

const std::vector<std::string>& decision_names() {
	static std::vector<std::string> names;
    static uint64_t generation = 0;
	if (names.empty() || generation != g_engine_context.generation()) {
        generation = g_engine_context.generation();
		names = read_strings("decision.str", true);
	}
	return names;
}

Creature* subject() {
	auto* obj = g_engine_context.objects->try_get(g_engine_context.world->current_norn);
	return obj ? obj->as_creature() : nullptr;
}

std::string creature_name(const Creature& c) {
	return c.history_name.empty() || c.history_name == "<Un Named>" ? c.moniker : c.history_name;
}

const char* sex_name(const Creature& c) {
	return c.sex == 2 ? "Female" : "Male";
}

const char* life_stage_name(int32_t age) {
	static const char* names[] = {"Baby", "Child", "Adolescent", "Youth", "Adult", "Old", "Senile", "Dead"};
	return names[std::clamp(age, 0, 7)];
}

std::string game_time(uint32_t ticks) {
	// a tick is a tenth of a second of game time
	uint32_t seconds = ticks / 10;
	return fmt::format("{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
}

uint8_t chemical(const Creature& c, size_t n) {
	return c.biochemistry && n < c.biochemistry->chemicals.size() ? c.biochemistry->chemicals[n].concentration : 0;
}

void chemical_bar(const Creature& c, size_t n) {
	const uint8_t value = chemical(c, n);
	ImGui::ProgressBar(value / 255.f, ImVec2(160, 0), fmt::format("{}", value).c_str());
	ImGui::SameLine();
	ImGui::TextUnformatted(chemical_names()[n].c_str());
}

// Most kits need a subject: "There is no subject" otherwise (Q11).
std::set<std::string> s_registration_loaded;
Creature* require_subject() {
	auto* c = subject();
    if (c && s_registration_loaded.insert(c->moniker).second) {
        try { load_registration(*c); }
        catch (const std::exception& e) { s_kit_error = e.what(); }
    }
	if (!c) {
		ImGui::TextUnformatted("There is no subject");
		ImGui::TextDisabled("Pick up a Norn or choose one from the Norns menu, then retry.");
		ImGui::Button("Retry");
	} else {
		ImGui::Text("Subject: %s (%s)", creature_name(*c).c_str(), c->moniker.c_str());
		ImGui::Separator();
	}
	return c;
}

fs::path pref_file(const char* name) {
	if (auto profile = session_profile(); !profile.empty()) { return profile / name; }
	char* dir = SDL_GetPrefPath("openc2e", "opencreatures1");
	if (!dir) {
		return {};
	}
	fs::path path = fs::path(dir) / name;
	SDL_free(dir);
	return path;
}

// A shop page (batch 4, Q26): Prev/Next, the item's picture, name, description, how many
// are left and ADD. ADD sends the item's script (which puts the new object in the hand) and
// takes one from the stock.
std::map<std::string, int32_t> s_shop_page;

void draw_shop(const std::string& file) {
	auto items = KitStock::load(file);
	if (items.empty()) {
		ImGui::TextDisabled("Couldn't read the file \"%s\" in the game folder.", file.c_str());
		return;
	}
	int32_t& page = s_shop_page[file];
	page = std::clamp(page, 0, static_cast<int32_t>(items.size()) - 1);
	auto& item = items[static_cast<size_t>(page)];

	// cycle through the items with the page-turn buttons
	if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) {
		page = (page + static_cast<int32_t>(items.size()) - 1) % static_cast<int32_t>(items.size());
	}
	ImGui::SameLine();
	ImGui::Text("%d of %zu", page + 1, items.size());
	ImGui::SameLine();
	if (ImGui::ArrowButton("##next", ImGuiDir_Right)) {
		page = (page + 1) % static_cast<int32_t>(items.size());
	}
	ImGui::Separator();

	const float scale = 2.0f;
	ImVec2 pos = ImGui::GetCursorScreenPos();
	// the picture is stored bottom row first, as in .cob files: read top row first, every
	// item showed upside down (checked against the sprites the ADD scripts make)
	std::vector<uint8_t> upright(item.pixels.size());
	const size_t row = static_cast<size_t>(std::max(0, item.width));
	for (size_t y = 0; row && y < static_cast<size_t>(item.height); ++y) {
		std::copy_n(item.pixels.begin() + static_cast<long>((static_cast<size_t>(item.height) - 1 - y) * row), row, upright.begin() + static_cast<long>(y * row));
	}
	const auto* picture = UIImages::indexed_image(fmt::format("{}:{}", file, page), item.width, item.height, upright);
	ImGui::Dummy(ImVec2(static_cast<float>(item.width) * scale, static_cast<float>(item.height) * scale));
	UIImages::draw_bitmap(ImGui::GetWindowDrawList(), picture, pos, scale);
	ImGui::SameLine();
	ImGui::BeginGroup();
	ImGui::TextUnformatted(item.name.c_str());
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240);
	ImGui::TextDisabled("%s", item.description.c_str());
	ImGui::PopTextWrapPos();
	ImGui::Text("Remaining: %u", item.stock);
	ImGui::BeginDisabled(item.stock == 0);
	if (ImGui::Button("ADD")) {
		// the script ends with EDIT, so the new object goes straight into the hand
		Macro m(item.script + ",endm");
		m.ownr = m.targ = g_engine_context.pointer->m_pointer_tool;
        try {
            g_engine_context.macros->run_now(m, false);
            item.stock--;
            KitStock::save(file, items);
        } catch (const std::exception& e) { s_kit_error = e.what(); }
	}
	ImGui::EndDisabled();
	if (item.stock == 0) {
		ImGui::TextDisabled("None left: Emergency (under Records) can restock them.");
	}
	ImGui::EndGroup();
}

// Each kit's first page is a picture, the "Cover" (batch 4, Q10; Jeff remembers it always
// coming first).
void cover_tab(const char* bitmap_name) {
	if (begin_page("Cover")) {
		if (const auto* cover = UIImages::bitmap(bitmap_name)) {
			const float fit = std::clamp((ImGui::GetContentRegionAvail().y - 4) / static_cast<float>(cover->height), 0.1f, 1.5f);
			ImVec2 pos = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(static_cast<float>(cover->width) * fit, static_cast<float>(cover->height) * fit));
			UIImages::draw_bitmap(ImGui::GetWindowDrawList(), cover, pos, fit);
		} else {
			ImGui::TextDisabled("(%s isn't in the game folder)", bitmap_name);
		}
		ImGui::EndTabItem();
	}
}

// --- Owner's Kit ---

void edit_string(const char* label, std::string& value) {
	char buffer[128];
	std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
	if (ImGui::InputText(label, buffer, sizeof(buffer))) {
		value = buffer;
        if (auto* c = subject()) {
            try { save_registration(*c); }
            catch (const std::exception& e) { s_kit_error = e.what(); }
        }
	}
}

// draws a kit picture (e.g. the birth certificate) with text over it
void picture_with_text(const char* bitmap_name, const std::vector<std::string>& lines, float scale = 1) {
	const auto* picture = UIImages::bitmap(bitmap_name);
	ImVec2 pos = ImGui::GetCursorScreenPos();
	const float w = (picture ? static_cast<float>(picture->width) : 240) * scale, h = (picture ? static_cast<float>(picture->height) : 300) * scale;
	ImGui::Dummy(ImVec2(w, h));
	auto* draw = ImGui::GetWindowDrawList();
	UIImages::draw_bitmap(draw, picture, pos, scale);
	// where the original writes on its pictures isn't known: the text goes in the middle
	const float line = ImGui::GetTextLineHeightWithSpacing();
	float y = pos.y + h / 2 - line * static_cast<float>(lines.size()) / 2;
	for (const auto& text : lines) {
		const float tw = ImGui::CalcTextSize(text.c_str()).x;
		draw->AddText(ImVec2(pos.x + (w - tw) / 2, y), IM_COL32(40, 28, 16, 255), text.c_str());
		y += line;
	}
}

// the Owner's page: who the Norn is, and the owner's record kept in it (Q12)
void owner_details(Creature& c, bool with_facts) {
	if (with_facts) {
		ImGui::Text("Name: %s", creature_name(c).c_str());
		ImGui::Text("Moniker: %s", c.moniker.c_str());
		ImGui::Text("Sex: %s", sex_name(c));
		ImGui::Text("Life stage: %s", c.dead ? "Dead" : life_stage_name(c.age));
		if (const auto* room = g_engine_context.map->find_room_containing(c.ground_x(), c.ground_y() - 1)) {
			ImGui::Text("Room: (%d,%d)-(%d,%d), type %d", room->left, room->top, room->right, room->bottom, room->type);
		} else {
			ImGui::TextDisabled("Room: outside defined rooms");
		}
		ImGui::Text("Age: %s", game_time(c.age_ticks).c_str());
		ImGui::SeparatorText("Register the birth");
	}
	// the creature's name and the 10-field owner record kept in the creature (Q12)
	edit_string("Name", c.history_name);
	edit_string("Birthday", c.history_birthday);
	edit_string("Birthplace", c.history_birthplace);
	edit_string("Owner's name", c.history_owner_name);
	edit_string("Owner's phone", c.history_owner_phone);
	edit_string("Owner's address", c.history_owner_address);
	edit_string("Owner's email", c.history_owner_email);
}

std::string parent_label(const std::string& moniker) {
	if (moniker.empty()) {
		return "unknown";
	}
	for (auto* obj : *g_engine_context.objects) {
		if (auto* p = obj->as_creature(); p && p->moniker == moniker) {
			return fmt::format("{} ({})", creature_name(*p), moniker);
		}
	}
	// no longer here: the family records may still know its name
	if (const auto* m = Family::find(moniker); m && !m->name.empty() && m->name != "<Un Named>") {
		return fmt::format("{} ({})", m->name, moniker);
	}
	return moniker;
}

// a meter filling the rest of the line, its label after it
void wide_meter(const std::string& label, float fraction, ImVec4 colour) {
	const float width = std::max(60.f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(label.c_str()).x - ImGui::GetStyle().ItemSpacing.x - 2);
	UIStyle::meter(label.c_str(), fraction, colour, width);
}

void certificate(const Creature& c, float scale = 1) {
	picture_with_text("birth.bmp", {
		creature_name(c),
		fmt::format("{} {}", sex_name(c), c.moniker),
		c.history_birthday.empty() ? std::string("") : "Born " + c.history_birthday,
		c.history_birthplace.empty() ? std::string("") : "at " + c.history_birthplace,
		"Mother: " + parent_label(c.history_moms_moniker),
		"Father: " + parent_label(c.history_dads_moniker),
		c.history_owner_name.empty() ? std::string("") : "Owner: " + c.history_owner_name,
	}, scale);
}

void draw_owners_kit() {
	auto* c = require_subject();
	if (!c || !ImGui::BeginTabBar("owner")) {
		return;
	}
	cover_tab("Owner.bmp");
	if (begin_page("Owner")) {
		owner_details(*c, true);
		ImGui::EndTabItem();
	}
	if (begin_page("Photographs")) {
		// the album (batch 4, Q14a): the camera takes a photo of the Norn
		PhotoAlbum::draw(c, c->moniker, true);
		ImGui::EndTabItem();
	}
	if (begin_page("Certificate")) {
		certificate(*c);
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

// --- Health Kit ---

void draw_lobe_activity(const Creature& c) {
	if (!c.brain || c.brain->lobes.empty()) {
		ImGui::TextDisabled("No brain lobes");
		return;
	}
	auto& brain = *c.brain;
	size_t first = 0;
	for (size_t l = 0; l < brain.lobes.size(); ++l) {
		auto& lobe = brain.lobes[l];
		uint32_t total = 0;
		for (size_t n = 0; n < lobe.num_neurons && first + n < brain.neurons.size(); ++n) {
			total += brain.neurons[first + n].output;
		}
		const float activity = lobe.num_neurons ? static_cast<float>(total) / (255.f * static_cast<float>(lobe.num_neurons)) : 0;
		ImGui::ProgressBar(std::min(1.f, activity * 4), ImVec2(160, 0), "");
		ImGui::SameLine();
		ImGui::Text("Lobe %zu: %u neurons (%ux%u)", l, lobe.num_neurons, lobe.width, lobe.height);
		first += lobe.num_neurons;
	}
}

void draw_add_object() {
	// the "Add Object" shop: injects COB objects (Q11)
	static int chosen = -1;
	auto cobs = list_injectable_cobs();
	ImGui::TextUnformatted("Choose an object file");
	if (ImGui::BeginListBox("##cobs", ImVec2(320, 160))) {
		for (int i = 0; i < static_cast<int>(cobs.size()); ++i) {
			if (ImGui::Selectable(cobs[static_cast<size_t>(i)].string().c_str(), chosen == i)) {
				chosen = i;
			}
		}
		ImGui::EndListBox();
	}
	// the chosen file's own picture and name, as the Injector shows them (batch 5, Q6)
	if (chosen >= 0 && chosen < static_cast<int>(cobs.size())) {
		static std::map<std::string, std::optional<c1cobfile>> s_cobs;
        static uint64_t generation = 0;
        if (generation != g_engine_context.generation()) {
            s_cobs.clear(); generation = g_engine_context.generation();
        }
		const std::string key = cobs[static_cast<size_t>(chosen)].string();
		auto it = s_cobs.find(key);
		if (it == s_cobs.end()) {
			std::optional<c1cobfile> cob;
			try {
				auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, key);
				if (!path.empty()) {
					cob = read_c1cobfile(path);
				}
			} catch (std::exception&) {
			}
			it = s_cobs.emplace(key, std::move(cob)).first;
		}
		if (it->second) {
			const auto& pic = it->second->picture;
			if (pic) {
				std::vector<uint8_t> pixels(pic.data.data(), pic.data.data() + static_cast<size_t>(pic.width) * static_cast<size_t>(pic.height));
				const auto* picture = UIImages::indexed_image("cob:" + key, pic.width, pic.height, pixels);
				const ImVec2 pos = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(static_cast<float>(pic.width) * 2, static_cast<float>(pic.height) * 2));
				UIImages::draw_bitmap(ImGui::GetWindowDrawList(), picture, pos, 2);
				ImGui::SameLine();
			}
			ImGui::TextWrapped("%s", it->second->name.c_str());
		}
	}
	ImGui::TextUnformatted(GameText::line(GameText::Original{42, 0xe3a138424e5403beull}, "Choose an object, then Add puts it into the world").c_str());
    int remaining = 0;
    if (chosen >= 0 && chosen < static_cast<int>(cobs.size())) {
        try { remaining = cob_remaining(cobs[static_cast<size_t>(chosen)]); }
        catch (const std::exception& e) { s_kit_error = e.what(); }
        if (remaining < 0) ImGui::TextUnformatted("Unlimited injections");
        else ImGui::Text("Remaining injections: %d", remaining);
    }
    ImGui::BeginDisabled(remaining == 0);
	if (ImGui::Button("Add") && chosen >= 0 && chosen < static_cast<int>(cobs.size())) {
		try {
			inject_cob(cobs[static_cast<size_t>(chosen)]);
		} catch (std::exception& e) {
            s_kit_error = e.what();
			fmt::print(stderr, "* Couldn't inject {:?}: {}\n", cobs[static_cast<size_t>(chosen)].string(), e.what());
		}
	}
    ImGui::EndDisabled();
    if (!s_kit_error.empty()) ImGui::TextWrapped("%s", s_kit_error.c_str());
}

void health_fitness(const Creature& c, float width = 200) {
	const int32_t health = creature_health_percent(c);
	UIStyle::meter(fmt::format("Health {}%", health).c_str(), static_cast<float>(health) / 100.f, UIStyle::health_colour(static_cast<float>(health) / 100.f), width);
	ImGui::Text("Life stage: %s", c.dead ? "Dead" : life_stage_name(c.age));
	if (const auto* room = g_engine_context.map->find_room_containing(c.ground_x(), c.ground_y() - 1)) {
		ImGui::Text("Room: (%d,%d)-(%d,%d), type %d", room->left, room->top, room->right, room->bottom, room->type);
	} else {
		ImGui::TextDisabled("Room: outside defined rooms");
	}
	ImGui::Text("%s", c.dead ? "Dead" : c.asleep ? "Asleep" : "Awake");
	ImGui::SeparatorText("Temperature");
	// what drives the original's thermometer isn't known; this shows the creature's
	// coldness and hotness chemicals
	chemical_bar(c, 4);
	chemical_bar(c, 5);
}

// every drive as a meter, in one or more columns
void health_drives(const Creature& c, int columns = 1, float width = 160) {
	if (ImGui::BeginTable("##drives", std::max(1, columns), ImGuiTableFlags_SizingStretchSame)) {
		for (size_t d = 0; d < c.loci.drives.size(); ++d) {
			ImGui::TableNextColumn();
			UIStyle::meter(Kits::chemical_name(d + 1).c_str(), c.loci.drives[d] / 255.f, UIStyle::health_colour(1.f - c.loci.drives[d] / 255.f), width);
		}
		ImGui::EndTable();
	}
}

void draw_health_kit() {
	auto* c = require_subject();
	if (!c || !ImGui::BeginTabBar("health")) {
		return;
	}
	// tab order and labels as the original's (batch 5, Q7)
	cover_tab("Health.bmp");
	if (begin_page("Fitness page")) {
		health_fitness(*c);
		ImGui::EndTabItem();
	}
	if (begin_page("Drives and needs")) {
		health_drives(*c);
		ImGui::EndTabItem();
	}
	if (begin_page("Brain activity")) {
		draw_lobe_activity(*c);
		ImGui::EndTabItem();
	}
	if (begin_page("Doctor's page")) {
		// herbs and food from the file "Health" (batch 4, Q26)
		draw_shop("Health");
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

// --- Science Kit ---

ChemicalHistory s_history;

void draw_brain_scanner(const Creature& c) {
	if (!c.brain) {
		ImGui::TextDisabled("No brain");
		return;
	}
	auto& brain = *c.brain;
	static int lobe_index = 0;
    lobe_index = std::clamp(lobe_index, 0, static_cast<int>(brain.lobes.size()) - 1);
    static int measure = 0;
    ImGui::Combo("Measure", &measure, "Output\0State\0Susceptibility\0Short-term weight\0Long-term weight\0Strength\0");
    static int connection_type = 0, connection = 0;
    if (measure >= 2) {
        ImGui::SliderInt("Connection type", &connection_type, 0, 1);
        ImGui::SliderInt("Connection number", &connection, 0, 254);
    }
	ImGui::SliderInt("Lobe", &lobe_index, 0, static_cast<int>(brain.lobes.size()) - 1);
	size_t first = 0;
	for (int l = 0; l < lobe_index; ++l) {
		first += brain.lobes[static_cast<size_t>(l)].num_neurons;
	}
	auto& lobe = brain.lobes[static_cast<size_t>(std::clamp(lobe_index, 0, static_cast<int>(brain.lobes.size()) - 1))];
	auto* draw = ImGui::GetWindowDrawList();
	ImVec2 origin = ImGui::GetCursorScreenPos();
	const float cell = 8;
	for (uint32_t n = 0; n < lobe.num_neurons && first + n < brain.neurons.size(); ++n) {
		const auto& neuron = brain.neurons[first + n];
		const float x = origin.x + static_cast<float>(n % std::max(1u, lobe.width)) * cell;
		const float y = origin.y + static_cast<float>(n / std::max(1u, lobe.width)) * cell;
        const uint8_t v = monitor_neuron(neuron, static_cast<BrainMeasure>(measure), static_cast<unsigned>(connection_type), static_cast<size_t>(connection));
		draw->AddRectFilled(ImVec2(x, y), ImVec2(x + cell - 1, y + cell - 1), IM_COL32(v, v / 2, 255 - v, 255));
	}
	ImGui::Dummy(ImVec2(static_cast<float>(lobe.width) * cell, static_cast<float>(lobe.height) * cell));
}

// The Science Kit's Decisions page: what the creature is doing and to what, and how strongly
// each decision is winning in the decision lobe.
void draw_decisions(Creature& c) {
	auto* it = g_engine_context.objects->try_get(c.attention);
	ImGui::Text("Now: %s%s%s", c.asleep ? "asleep" : decision_name(c.current_decision).c_str(), it ? ", " : "", it ? get_object_name(it).c_str() : "");
	if (!c.brain) {
		ImGui::TextDisabled("No brain");
		return;
	}
	auto& brain = *c.brain;
	size_t first = 0;
	for (size_t l = 0; l < LOBE_DECISION && l < brain.lobes.size(); ++l) {
		first += brain.lobes[l].num_neurons;
	}
	if (brain.lobes.size() <= LOBE_DECISION) {
		return;
	}
	const auto& lobe = brain.lobes[LOBE_DECISION];
	const auto& names = decision_names();
	for (uint32_t d = 0; d < lobe.num_neurons && d < names.size() && first + d < brain.neurons.size(); ++d) {
		ImGui::ProgressBar(brain.neurons[first + d].output / 255.f, ImVec2(160, 0), "");
		ImGui::SameLine();
		ImGui::TextUnformatted(names[d].c_str());
	}
}

// four chemicals at a time, chosen by group (themes.str) or one by one
void science_chemistry(const Creature& c, float plot_width = 320) {
	static std::vector<Theme> themes;
	static uint64_t generation = 0;
	if (generation != g_engine_context.generation()) {
		themes = read_themes();
		generation = g_engine_context.generation();
	}
	static int theme = 0;
	static std::array<int, ChemicalHistory::channels> chosen{1, 2, 3, 4};
	if (!themes.empty() && ImGui::BeginCombo("Chemical group", themes[static_cast<size_t>(theme)].name.c_str())) {
		for (int t = 0; t < static_cast<int>(themes.size()); ++t) {
			if (ImGui::Selectable(themes[static_cast<size_t>(t)].name.c_str(), t == theme)) {
				theme = t;
				for (size_t i = 0; i < 4; ++i) {
					chosen[i] = themes[static_cast<size_t>(t)].chemicals[i];
				}
			}
		}
		ImGui::EndCombo();
	}
	const float plot_height = std::max(32.f, std::min(64.f, (ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 4) / 4 - 6));
	for (size_t i = 0; i < 4; ++i) {
		ImGui::PushID(static_cast<int>(i));
		auto& n = chosen[i];
		ImGui::SetNextItemWidth(plot_width);
		if (ImGui::BeginCombo("##chem", chemical_names()[static_cast<size_t>(n)].c_str())) {
			for (int k = 0; k < 256; ++k) {
				if (ImGui::Selectable(chemical_names()[static_cast<size_t>(k)].c_str(), k == n)) {
					n = k;
				}
			}
			ImGui::EndCombo();
		}
		auto& samples = s_history.samples[static_cast<size_t>(n)];
		std::vector<float> values(samples.begin(), samples.end());
		ImGui::PlotLines("##plot", values.data(), static_cast<int>(values.size()), 0, fmt::format("{}", chemical(c, static_cast<size_t>(n))).c_str(), 0, 255, ImVec2(plot_width, plot_height));
		ImGui::PopID();
	}
}

void science_genes(const Creature& c, ImVec2 size = ImVec2(360, 260)) {
	ImGui::Text("Species: %s (genus %u, classifier species %u)", c.genus == 1 ? "Norn" : c.genus == 2 ? "Grendel" : "Other creature", c.genus, c.species);
	if (!c.genome) {
		ImGui::TextDisabled("No genome");
	} else if (ImGui::BeginListBox("##genes", size)) {
		int i = 0;
		for (auto& g : c.genome->genes) {
			ImGui::Text("%3d  %s", i++, g->name());
		}
		ImGui::EndListBox();
	}
}

// Batch 4, Q26: the Science Kit injects the mixtures named in injections.str; mixture k
// is chemical 100 + k, and a dose d gives d / 8 of it.
const std::vector<std::string>& injection_mixtures() {
	static std::vector<std::string> mixtures;
	static uint64_t generation = 0;
	if (generation != g_engine_context.generation()) {
		mixtures = read_strings("injections.str", false);
		mixtures.resize(std::min<size_t>(mixtures.size(), 156));
		generation = g_engine_context.generation();
	}
	return mixtures;
}

void inject_mixture_into(Creature& c, size_t mixture, int dose) {
	const size_t chem = 100 + mixture;
	if (c.biochemistry && chem < c.biochemistry->chemicals.size()) {
		auto& value = c.biochemistry->chemicals[chem].concentration;
		value = static_cast<uint8_t>(std::min(255, value + dose / 8));
	}
}

void science_injections(Creature& c, bool as_list = true) {
	const auto& mixtures = injection_mixtures();
	static int mixture = 0;
	static int dose = 128;
	mixture = std::clamp(mixture, 0, std::max(0, static_cast<int>(mixtures.size()) - 1));
	if (as_list) {
		for (int k = 0; k < static_cast<int>(mixtures.size()); ++k) {
			if (ImGui::RadioButton(mixtures[static_cast<size_t>(k)].c_str(), mixture == k)) {
				mixture = k;
			}
		}
	} else if (!mixtures.empty() && ImGui::BeginCombo("Mixture", mixtures[static_cast<size_t>(mixture)].c_str())) {
		for (int k = 0; k < static_cast<int>(mixtures.size()); ++k) {
			if (ImGui::Selectable(mixtures[static_cast<size_t>(k)].c_str(), mixture == k)) {
				mixture = k;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SliderInt("Dose", &dose, 8, 255);
	const size_t chem = 100 + static_cast<size_t>(mixture);
	ImGui::Text("%s now: %d", chemical_names()[chem].c_str(), chemical(c, chem));
	if (ImGui::Button("Inject") && !mixtures.empty()) {
		inject_mixture_into(c, static_cast<size_t>(mixture), dose);
	}
	if (ImGui::CollapsingHeader("Any chemical (not in the original)")) {
		static int any = 1;
		static int amount = 50;
		if (ImGui::BeginCombo("Chemical", chemical_names()[static_cast<size_t>(any)].c_str())) {
			for (int k = 1; k < 256; ++k) {
				if (ImGui::Selectable(chemical_names()[static_cast<size_t>(k)].c_str(), k == any)) {
					any = k;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SliderInt("Amount", &amount, 1, 255);
		if (ImGui::Button("Inject this") && c.biochemistry) {
			auto& value = c.biochemistry->chemicals[static_cast<size_t>(any)].concentration;
			value = static_cast<uint8_t>(std::min(255, value + amount));
		}
	}
}

void draw_science_kit() {
	auto* c = require_subject();
	if (!c) {
		return;
	}
	s_history.sample(*c, g_engine_context.world->tick);
	if (!ImGui::BeginTabBar("science")) {
		return;
	}
	// tab order and labels as the original's (batch 5, Q7)
	cover_tab("Science.bmp");
	if (begin_page("Biochemistry")) {
		science_chemistry(*c);
		ImGui::EndTabItem();
	}
	if (begin_page("Genetics")) {
		science_genes(*c);
		ImGui::EndTabItem();
	}
	if (begin_page("Brain scanner")) {
		draw_brain_scanner(*c);
		ImGui::EndTabItem();
	}
	if (begin_page("Decisions")) {
		draw_decisions(*c);
		ImGui::EndTabItem();
	}
	if (begin_page("Injections")) {
		science_injections(*c);
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

// --- Breeder's Kit ---

// months pregnant as the original reports it: ceiling(progesterone / 32), 1-8 (batch 4, Q26)

// chemicals 13, 59 and 65, and 63 and 66 for females or 59 and 64 for males (Q11)
void breeder_fertility(const Creature& c) {
	ImGui::Text("Sex: %s", sex_name(c));
	if (c.sex != 2) {
		// the male picture: he can't carry an egg, but he can still father a child
		if (const auto* male = UIImages::bitmap("Male.bmp")) {
			ImVec2 pos = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(static_cast<float>(male->width), static_cast<float>(male->height)));
			UIImages::draw_bitmap(ImGui::GetWindowDrawList(), male, pos, 1);
			ImGui::SameLine();
		}
	}
	ImGui::BeginGroup();
	chemical_bar(c, 13);
	chemical_bar(c, 65);
	if (c.sex == 2) {
		chemical_bar(c, 63);
		chemical_bar(c, 66);
	} else {
		chemical_bar(c, 59);
		chemical_bar(c, 64);
	}
	ImGui::EndGroup();
	ImGui::SeparatorText("Pregnancy");
	if (c.sex != 2) {
		ImGui::TextUnformatted("Male: he can't carry an egg.");
	} else if (const int32_t months = creature_gestation_stage(c)) {
		ImGui::Text("Pregnant: month %d of 8", months);
		UIStyle::meter("", static_cast<float>(months) / 8.f, UIStyle::health_colour(1.f), 200);
	} else {
		ImGui::TextUnformatted("Not pregnant.");
	}
}

void draw_breeders_kit() {
	auto* c = require_subject();
	if (!c || !ImGui::BeginTabBar("breeder")) {
		return;
	}
	// tab order and labels as the original's; the pregnancy display is on the fertility page
	// (batch 5, Q7)
	cover_tab("Sex.bmp");
	if (begin_page("Fertility page")) {
		breeder_fertility(*c);
		ImGui::EndTabItem();
	}
	if (begin_page("Aphrodisiac page")) {
		// Tomato and Ugly Tomato from the file "Aphro" (batch 4, Q26)
		draw_shop("Aphro");
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

// --- The Hatchery (Q13) ---

// The egg record: six characters, '0' or '1' for an egg still there and 'x' once taken.
// Exactly three of each digit are chosen at random when it's reset. It's kept with the
// player's options, as the original keeps it in the registry.
std::string s_eggs;

void load_egg_record() {
	std::ifstream in(pref_file("hatchery.txt"));
	std::getline(in, s_eggs);
	if (s_eggs.size() != 6 || s_eggs == "xxxxxx" || s_eggs == "empty") {
		// six new eggs, exactly three of each sex in random slots
		s_eggs = "000111";
		for (size_t i = 5; i > 0; --i) {
			std::swap(s_eggs[i], s_eggs[static_cast<size_t>(rand_int32(0, static_cast<int32_t>(i)))]);
		}
        try { write_persistent_file(pref_file("hatchery.txt"), s_eggs + "\n"); }
        catch (const std::exception& e) { s_kit_error = e.what(); }
	}
}

void save_egg_record() {
    try { write_persistent_file(pref_file("hatchery.txt"), s_eggs + "\n"); }
    catch (const std::exception& e) { s_kit_error = e.what(); }
}

void take_egg(size_t slot, bool female) {
	// Batch 4, Q12: the Hatchery sends the game this script. The egg (eggs.spr frames 8n to
	// 8n+7, class 2 5 2) appears by the incubator, its genome a child of mum<n+1> and
	// dad<n+1>, its sex code in OBV1, and the camera moves there.
	const int32_t n = static_cast<int32_t>(slot);
	Macro m(fmt::format(
		"inst,sys: wtop,sys: cmra 2223 724,new: simp eggs 8 {} 2000 0,pose 3,setv clas 33882624,setv attr 67,"
		"new: gene tokn mum{} tokn dad{} obv0,setv obv1 {},tick 2400,dde: hatc,mvto {} 870,endm",
		8 * n, n + 1, n + 1, female ? 2 : 1, 2408 + 40 * n));
	m.ownr = m.targ = g_engine_context.pointer->m_pointer_tool;
	g_engine_context.macros->add(m);
}

// the Hatchery's picture (batch 4, Q12): a 320 x 240 scene animated on a 75 ms timer
struct HatcheryScene {
	double next_tick = 0;
	int32_t ticks = 0;
	bool lamp_lit = false;
	int32_t hovered = -1;
	int32_t wobble_left = 0;
	C1ControlledSound sex_sound;
};
HatcheryScene s_hatchery;

constexpr std::array<ImVec2, 6> EGG_POSITIONS = {{{60, 158}, {90, 149}, {122, 162}, {150, 159}, {186, 152}, {205, 155}}};

void play_kit_sound(const char* name) {
	g_engine_context.sounds->play_uncontrolled_sound(name, Rect2f{});
}

void draw_hatchery(float fit_scale = 0) {
    if (s_eggs == "xxxxxx" && ImGui::Button("Refill nest")) {
        load_egg_record();
    }
	const bool privileged = g_engine_context.pointer->m_editing;
	if (s_eggs.empty()) {
		load_egg_record();
	}
	auto& scene = s_hatchery;
	const double now = ImGui::GetTime();
	if (now >= scene.next_tick) {
		scene.next_tick = now + 0.075;
		scene.ticks++;
		// the lamp: off 9 ticks in 10, the other lit with a flicker sound
		const bool lit = rand_int32(0, 9) == 0;
		if (lit && !scene.lamp_lit) {
			play_kit_sound("hlgt");
		}
		scene.lamp_lit = lit;
		if (scene.wobble_left > 0) {
			scene.wobble_left--;
		}
	}

	// 1.5x in the modern interface (or as large as fits a pane); the original's own size in
	// classic mode
	const float scale = fit_scale > 0 ? fit_scale : UIStyle::classic() ? 1.0f : UIStyle::scale() * 1.5f;
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##scene", ImVec2(320 * scale, 240 * scale));
	auto* draw = ImGui::GetWindowDrawList();
	auto at = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
	using UIImages::bitmap;
	using UIImages::draw_bitmap;
	draw_bitmap(draw, bitmap("Hatchery/hatchery.bmp"), at(0, 0), scale);
	draw_bitmap(draw, bitmap(fmt::format("Hatchery/FAN{}.BMP", scene.ticks % 4)), at(229, 54), scale);
	if (!scene.lamp_lit) {
		draw_bitmap(draw, bitmap("Hatchery/LIGHTOFF.BMP"), at(109, 0), scale);
	}

	// which egg is under the mouse: the front-most (the last drawn) wins
	auto present = [&](size_t i) { return privileged || s_eggs[i] != 'x'; };
	int32_t hovered = -1;
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	if (ImGui::IsItemHovered()) {
		for (int32_t i = 5; i >= 0; --i) {
			const auto* egg = bitmap(fmt::format("Hatchery/EGG{}.bmp", i));
			const ImVec2 p = at(EGG_POSITIONS[static_cast<size_t>(i)].x, EGG_POSITIONS[static_cast<size_t>(i)].y);
			if (egg && present(static_cast<size_t>(i)) && mouse.x >= p.x && mouse.y >= p.y && mouse.x < p.x + static_cast<float>(egg->width) * scale && mouse.y < p.y + static_cast<float>(egg->height) * scale) {
				hovered = i;
				break;
			}
		}
	}
	const bool female_egg = hovered >= 0 && (privileged ? (hovered % 2 == 1) : s_eggs[static_cast<size_t>(hovered)] == '1');
	if (hovered != scene.hovered) {
		// moving onto an egg: it wobbles, "hegg" plays and its sex is heard and shown
		scene.sex_sound.stop();
		scene.sex_sound = {};
		if (hovered >= 0) {
			scene.wobble_left = rand_int32(5, 9);
			play_kit_sound("hegg");
			scene.sex_sound = g_engine_context.sounds->play_controlled_sound(female_egg ? "hfml" : "hmle", Rect2f{}, true);
		}
		scene.hovered = hovered;
	}
	for (size_t i = 0; i < 6; ++i) {
		if (!present(i)) {
			continue;
		}
		ImVec2 p = EGG_POSITIONS[i];
		if (static_cast<int32_t>(i) == hovered && scene.wobble_left > 0) {
			p.x += (scene.wobble_left % 2 ? 2.f : -2.f);
		}
		draw_bitmap(draw, bitmap(fmt::format("Hatchery/EGG{}.bmp", i)), at(p.x, p.y), scale);
	}
	draw_bitmap(draw, bitmap("Hatchery/HTCHMASK.bmp"), at(0, 169), scale);
	if (hovered >= 0) {
		// the spinning symbol: 28 x 24 cells, 6 frames 29 px apart, a step every 2 ticks;
		// the top row is female, the second male
		const auto* egg = bitmap(fmt::format("Hatchery/EGG{}.bmp", hovered));
		const ImVec2 p = EGG_POSITIONS[static_cast<size_t>(hovered)];
		const float x = p.x + static_cast<float>(egg ? egg->width : 30) / 2 - 14;
		const int32_t frame = (scene.ticks / 2) % 6;
		draw_bitmap(draw, bitmap("Hatchery/GENSPIN.bmp"), at(x, p.y - 26), scale, Rect2i{frame * 29, female_egg ? 0 : 25, 28, 24});
		ImGui::SetTooltip("A %s egg. Double-click to put it into Albia.", female_egg ? "female" : "male");
		if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			play_kit_sound("hslt");
			take_egg(static_cast<size_t>(hovered), female_egg);
			if (!privileged) {
				s_eggs[static_cast<size_t>(hovered)] = 'x';
				save_egg_record();
			}
			scene.sex_sound.stop();
			scene.sex_sound = {};
			scene.hovered = -1;
		}
	}

	const auto left = std::count_if(s_eggs.begin(), s_eggs.end(), [](char c) { return c != 'x'; });
	if (privileged) {
		ImGui::TextDisabled("Privileged: the eggs never run out");
	} else if (left == 0) {
		// after the last egg the record reads xxxxxx, and the next opening makes six new ones
		ImGui::TextUnformatted("The nest is empty. Refill it using the button above.");
	} else {
		ImGui::TextDisabled("%ld egg%s left. Hover over an egg to see whether it's male or female.", static_cast<long>(left), left == 1 ? "" : "s");
	}
}

// --- Funeral Kit ---

using Grave = GraveRecord;
std::vector<Grave> s_graves;
std::set<std::string> s_buried;
bool s_graves_loaded = false;
bool s_graves_writable = true;
bool s_graves_dirty = false;

// Each world has its own graveyard, next to its world file (World.openc1.sfc ->
// World.openc1.graveyard.txt). A world without one yet starts from the graveyard all worlds
// used to share, which is kept as it was.
std::filesystem::path graveyard_path() {
	const auto folder = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "");
	return folder / (std::filesystem::path(get_world_file_name()).stem().string() + ".graveyard.txt");
}

void load_graves() {
	s_graves_loaded = true;
	s_graves.clear();
	s_buried.clear();
    s_graves_writable = true;
    try {
        s_graves = load_graveyard(fs::exists(graveyard_path()) ? graveyard_path() : pref_file("graveyard.txt"));
        for (const auto& g : s_graves) s_buried.insert(g.moniker);
    } catch (const std::exception& e) { s_graves_writable = false; s_kit_error = e.what(); }
}

void save_graves() {
    s_graves_dirty = true;
    if (!s_graves_writable) return; // preserve an unreadable/corrupt input for recovery
    try { save_graveyard(graveyard_path(), s_graves); s_graves_dirty = false; }
    catch (const std::exception& e) { s_kit_error = e.what(); }
}

void record_deaths() {
	// The Funeral Kit learns of deaths from death row (Q31). Dead creatures still in the
	// world are recorded too, in case no script takes them off the event list.
	if (!s_graves_loaded) {
		load_graves();
	}
    bool changed = s_graves_dirty;
    for (const auto& snapshot : g_engine_context.world->pending_graves) {
        if (!s_buried.insert(snapshot.moniker).second) continue;
        s_graves.push_back(snapshot);
        try {
            const auto photos = PhotoAlbum::load(snapshot.moniker);
            s_graves.back().photo = photos.empty() ? -1 : static_cast<int32_t>(photos.size()) - 1;
        } catch (const std::exception& e) { s_kit_error = e.what(); }
        changed = true;
    }
    g_engine_context.world->pending_graves.clear();
	auto bury = [&](Creature& c) {
		if (s_buried.count(c.moniker)) {
			return;
		}
		s_buried.insert(c.moniker);
		Grave g{c.moniker, creature_name(c), sex_name(c), game_time(c.age_ticks), "", -1};
        g.born = c.history_birthday;
		// its latest photo goes on the headstone to start with
        std::vector<PhotoAlbum::Photo> photos;
        try { photos = PhotoAlbum::load(c.moniker); }
        catch (const std::exception& e) { s_kit_error = e.what(); }
		g.photo = photos.empty() ? -1 : static_cast<int32_t>(photos.size()) - 1;
		// the registry: its parents by name where the family registry knows them, and the date
		auto parent = [](const std::string& m) -> std::string {
			if (m.empty() || m == "EVE_" || m == "ADAM" || m.rfind("mum", 0) == 0 || m.rfind("dad", 0) == 0) {
				return "the Hatchery";
			}
			const auto* p = Family::find(m);
			return p && !p->name.empty() && p->name != "<Un Named>" ? p->name : m;
		};
		g.parents = c.mother == c.father || parent(c.mother) == "the Hatchery" ? parent(c.mother) : parent(c.mother) + " and " + parent(c.father);
		{
			std::time_t now = std::time(nullptr);
			char date[64];
			std::strftime(date, sizeof(date), "%d %B %Y, %H:%M", std::localtime(&now));
			g.died = date;
		}
		s_graves.push_back(g);
		changed = true;
	};
	auto& death_row = g_engine_context.world->death_row;
	for (auto handle : death_row) {
		if (auto* obj = g_engine_context.objects->try_get(handle); obj && obj->as_creature()) {
			bury(*obj->as_creature());
		}
	}
	death_row.clear();
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature(); c && c->dead) {
			bury(*c);
		}
	}
	if (changed) {
		save_graves();
	}
}

void draw_headstone(Grave& g) {
	// the headstone (Grave.bmp) with the photo chosen for it, the name and the inscription;
	// creatures nobody named get the unmarked grave (Unknown.bmp)
	const bool unmarked = g.name.empty() || g.name == g.moniker;
	const auto* stone = UIImages::bitmap(unmarked ? "Unknown.bmp" : "Grave.bmp");
	ImVec2 pos = ImGui::GetCursorScreenPos();
	const float w = stone ? static_cast<float>(stone->width) : 240, h = stone ? static_cast<float>(stone->height) : 302;
	ImGui::Dummy(ImVec2(w, h));
	auto* draw = ImGui::GetWindowDrawList();
	UIImages::draw_bitmap(draw, stone, pos, 1);
	if (unmarked) {
		const char* text = "No Record";
		draw->AddText(ImVec2(pos.x + (w - ImGui::CalcTextSize(text).x) / 2, pos.y + h * 0.55f), IM_COL32(230, 225, 210, 255), text);
		return;
	}
	float y = pos.y + h * 0.18f;
    std::vector<PhotoAlbum::Photo> photos;
    try { photos = PhotoAlbum::load(g.moniker); }
    catch (const std::exception& e) { s_kit_error = e.what(); }
	if (g.photo >= 0 && g.photo < static_cast<int32_t>(photos.size())) {
		const auto& p = photos[static_cast<size_t>(g.photo)];
		const float scale = 0.6f;
		const auto* picture = UIImages::indexed_image(fmt::format("photo:{}:{}:{}", g.moniker, g.photo, p.time), p.width, p.height, p.pixels);
		UIImages::draw_bitmap(draw, picture, ImVec2(pos.x + (w - static_cast<float>(p.width) * scale) / 2, y), scale);
		y += static_cast<float>(p.height) * scale + 6;
	}
	for (const std::string& text : {g.name, g.age, g.headstone}) {
		draw->AddText(ImVec2(pos.x + (w - ImGui::CalcTextSize(text.c_str()).x) / 2, y), IM_COL32(230, 225, 210, 255), text.c_str());
		y += ImGui::GetTextLineHeightWithSpacing();
	}
}

std::string s_grave_selection;

int chosen_grave() {
	for (size_t i = 0; i < s_graves.size(); ++i) {
		if (s_graves[i].moniker == s_grave_selection) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

// the list of graves, and the chosen one's headstone and record
void funeral_graves(float list_height = 300) {
	int chosen = chosen_grave();
	if (s_graves.empty()) {
		ImGui::TextDisabled("No one has died yet.");
	}
	ImGui::BeginChild("##graves", ImVec2(200, list_height));
	for (int i = 0; i < static_cast<int>(s_graves.size()); ++i) {
		auto& g = s_graves[static_cast<size_t>(i)];
		if (ImGui::Selectable(fmt::format("{}##{}", g.name, i).c_str(), chosen == i)) {
			chosen = i;
			s_grave_selection = g.moniker;
		}
		ImGui::SetItemTooltip("%s, %s, lived %s", g.moniker.c_str(), g.sex.c_str(), g.age.c_str());
	}
	ImGui::EndChild();
	if (chosen >= 0 && chosen < static_cast<int>(s_graves.size())) {
		ImGui::SameLine();
		ImGui::BeginGroup();
		auto& g = s_graves[static_cast<size_t>(chosen)];
		draw_headstone(g);
		// the registry
		ImGui::TextDisabled("%s, %s, lived %s", g.moniker.c_str(), g.sex.c_str(), g.age.c_str());
		if (!g.parents.empty()) {
			ImGui::TextDisabled("Child of %s", g.parents.c_str());
		}
		ImGui::TextWrapped("Born %s", g.born.empty() ? "unknown (not recorded)" : g.born.c_str());
		if (!g.died.empty()) {
			ImGui::TextWrapped("Died %s", g.died.c_str());
		}
		ImGui::TextUnformatted("Make Headstone");
		char text[128];
		std::snprintf(text, sizeof(text), "%s", g.headstone.c_str());
		ImGui::SetNextItemWidth(240);
		if (ImGui::InputText("##headstone", text, sizeof(text))) {
			g.headstone = text;
			save_graves();
		}
		ImGui::EndGroup();
	}
}

void funeral_photos() {
	const int chosen = chosen_grave();
	if (chosen >= 0 && chosen < static_cast<int>(s_graves.size())) {
		auto& g = s_graves[static_cast<size_t>(chosen)];
		ImGui::Text("Photographs of %s", g.name.c_str());
		const int32_t shown = PhotoAlbum::draw(nullptr, g.moniker, false);
		ImGui::BeginDisabled(shown < 0);
		if (ImGui::Button("Put this photo on the headstone")) {
			g.photo = shown;
			save_graves();
		}
		ImGui::EndDisabled();
	} else {
		ImGui::TextDisabled("Choose a grave first.");
	}
}

void draw_funeral_kit() {
	if (!ImGui::BeginTabBar("funeral")) {
		return;
	}
	cover_tab("Funeral.bmp");
	if (begin_page("GraveYard")) {
		funeral_graves();
		ImGui::EndTabItem();
	}
	if (begin_page("Photographs")) {
		funeral_photos();
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

// --- Score Kit ---

void score_page() {
	auto& w = *g_engine_context.world;
	ImGui::Text("Elapsed Time: %s", game_time(w.tick).c_str());
	ImGui::Text("Current Norns: %u", w.live_norns);
	ImGui::Text("Previous Norns: %u", w.dead_norns);
	ImGui::Text("Hatchery Eggs: %u", w.hatchery_eggs);
	ImGui::Text("Natural Eggs: %u", w.natural_eggs);
	ImGui::Separator();
	ImGui::SetWindowFontScale(1.4f);
	ImGui::Text("Breeders Score: %u", player_score());
	ImGui::SetWindowFontScale(1.0f);
}

void draw_score_kit() {
	// the score counters (batch 3, Q30); the score itself is 256 x natural eggs plus
	// creature-time, at most 99999 (batch 4, Q26)
	if (!ImGui::BeginTabBar("score")) {
		return;
	}
	cover_tab("Score.bmp");
	if (begin_page("Score")) {
		score_page();
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

// --- Observation Kit ---

// The Observation Kit's options and alerts (batch 4, Q26). The default threshold isn't known.
ObservationOptions s_observation;
ObservationHistory s_alerted;
std::vector<std::string> s_alerts;

void alert(const std::string& text) {
    // Classic SDL message boxes block this thread and prevent population refresh.
    // Keep every native alert in a bounded, nonmodal notification panel instead.
    s_alerts.push_back(text);
    if (s_alerts.size() > 8) s_alerts.erase(s_alerts.begin());
    if (s_observation.message_box) s_open[KIT_OBSERVATION] = true;
}

const std::vector<ObjectHandle>& population() {
    static uint64_t generation = 0, revision = 0;
    static std::vector<ObjectHandle> handles;
    if (generation != g_engine_context.generation() || revision != g_engine_context.objects->revision()) {
        handles.clear();
        for (auto* obj : *g_engine_context.objects) if (obj->as_creature()) handles.push_back(obj->uid);
        generation = g_engine_context.generation();
        revision = g_engine_context.objects->revision();
    }
    return handles;
}

void check_alerts() {
    for (auto handle : population()) {
        auto* obj = g_engine_context.objects->try_get(handle);
        if (!obj) continue;
		auto* c = obj->as_creature();
		if (!c || c->dead) {
			continue;
		}
        for (auto event : s_alerted.poll(c->moniker, c->dead, creature_health_percent(*c), creature_gestation_stage(*c), s_observation)) {
            // the Observation Kit's own words when the game has them
            const std::string description = event == ObservationHistory::LOW_LIFE ? GameText::line(GameText::Original{34, 0x680db9b33fd2451eull}, "is dangerously weak.")
                : event == ObservationHistory::PREGNANT ? GameText::line(GameText::Original{26, 0x670e259297c90216ull}, "is expecting an egg.") : GameText::line(GameText::Original{24, 0xeae8747268e59ab6ull}, "will lay an egg very soon.");
            alert(fmt::format("{} {}", creature_name(*c), description));
        }
	}
}

void observation_details() {
	for (auto& a : s_alerts) {
		ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1), "(!) %s", a.c_str());
	}
	if (!s_alerts.empty() && ImGui::SmallButton("Clear alerts")) {
		s_alerts.clear();
	}
        // Rebuild from current identity handles; there is no fixed population limit.
	if (ImGui::BeginTable("overview", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, std::max(120.f, ImGui::GetContentRegionAvail().y - 12)))) {
		for (const char* h : {"Name", "Age", "Pregnant", "Life Force", "Medical", "Sex"}) {
			ImGui::TableSetupColumn(h);
		}
		ImGui::TableHeadersRow();
            if (std::getenv("OPENC1_KIT_CHECK") && !session_profile().empty()) ImGui::SetScrollY(100000.f);
            const auto& rows = population();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows.size()));
            while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                static bool reached_end = false;
                if (!reached_end && rows.size() > 13 && row + 1 == static_cast<int>(rows.size()) && std::getenv("OPENC1_KIT_CHECK")) {
                    reached_end = true;
                    fmt::print("TEST K021 PASS: reached final population row {}\n", rows.size());
                }
                auto* obj = g_engine_context.objects->try_get(rows[static_cast<size_t>(row)]);
                auto* c = obj ? obj->as_creature() : nullptr;
			if (!c) {
				continue;
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(c->moniker.c_str());
			if (ImGui::Selectable(creature_name(*c).c_str(), c->uid == g_engine_context.world->current_norn, ImGuiSelectableFlags_SpanAllColumns)) {
				g_engine_context.world->current_norn = c->uid;
			}
			ImGui::PopID();
			ImGui::TableNextColumn();
			ImGui::Text("%u:%02u", c->age_ticks / 36000, c->age_ticks / 600 % 60);
			ImGui::TableNextColumn();
			if (c->sex != 2) {
				ImGui::TextDisabled("N/A");
			} else if (const int32_t months = creature_gestation_stage(*c)) {
				ImGui::Text("%d month%s", months, months == 1 ? "" : "s");
			} else {
				ImGui::TextUnformatted("No");
			}
			ImGui::TableNextColumn();
			const int32_t life = creature_health_percent(*c);
			if (c->dead || life == 0) {
				ImGui::TextUnformatted("Dead");
			} else {
				ImGui::TextColored(UIStyle::health_colour(static_cast<float>(life) / 100.f), "%d%%", life);
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(c->dead ? "-" : c->bacterium.state ? "Ill" : c->asleep ? "Asleep" : "Healthy");
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(sex_name(*c));
		}
		ImGui::EndTable();
	}
}

void observation_options() {
	ImGui::Checkbox(GameText::line(GameText::Original{44, 0xe282a71dfa905453ull}, "Warn when a Norn's life force drops under (%):").c_str(), &s_observation.warn_life_force);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(100);
	ImGui::InputInt("##threshold", &s_observation.life_force_warning);
	s_observation.life_force_warning = std::clamp(s_observation.life_force_warning, 0, 100);
	ImGui::Checkbox(GameText::line(GameText::Original{36, 0x3b576cc317d04557ull}, "Tell me when a Norn becomes pregnant.").c_str(), &s_observation.warn_pregnant);
	ImGui::Checkbox(GameText::line(GameText::Original{39, 0x39958373f4cd9645ull}, "Tell me when a Norn is about to lay an egg.").c_str(), &s_observation.warn_birth);
	ImGui::Checkbox("Show alerts in the notification panel", &s_observation.message_box);
        save_preference("observe.threshold", s_observation.life_force_warning);
        save_preference("observe.low", s_observation.warn_life_force);
        save_preference("observe.pregnant", s_observation.warn_pregnant);
        save_preference("observe.birth", s_observation.warn_birth);
        save_preference("observe.message", s_observation.message_box);
}

void draw_observation_kit() {
    ImGui::TextDisabled("%s", GameUI::paused() ? "Simulation paused; monitoring current state" : "Monitoring live simulation");
	check_alerts();
	if (!ImGui::BeginTabBar("observation")) {
		return;
	}
	if (begin_page("Details")) {
		observation_details();
		ImGui::EndTabItem();
	}
	if (begin_page("Options")) {
		observation_options();
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

// --- Backup and Emergency ---

std::string s_backup_message;

void draw_backup_kit() {
	// the Emergency Kit (batch 4, Q13 and Q26)
	const std::string name = get_world_file_name();
	auto backup = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "Backup/" + name);
	ImGui::TextWrapped("Recovery saves appear in World > Load world. Three autosaves are retained beside each manual save. This page also restores older Backup-folder saves.");
	ImGui::Spacing();
	const ImVec2 button(220, 0);

	ImGui::BeginDisabled(backup.empty());
	if (ImGui::Button("Restore World", button)) {
		ImGui::OpenPopup("Restore World");
	}
	ImGui::EndDisabled();
	if (backup.empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("(no backup of %s yet)", name.c_str());
	}
	if (ImGui::BeginPopupModal("Restore World", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(GameText::line(GameText::Original{43, 0xf8dda8b75727cf1aull}, "Restore this world from its backup?").c_str());
		if (ImGui::Button("OK", ImVec2(100, 0))) {
			// the backed-up creature pictures go back, then the backed-up world loads
			std::error_code err;
			auto images = g_engine_context.paths->find_path(PATH_TYPE_IMAGE, "");
			for (auto& entry : fs::directory_iterator(backup.parent_path(), err)) {
				if (entry.path().extension() == ".spr" && !images.empty()) {
					fs::copy_file(entry.path(), images / entry.path().filename(), fs::copy_options::overwrite_existing, err);
				}
			}
			s_backup_message = "Restored " + name + " from Backup.";
			GameUI::request_world_load("Backup/" + name, name);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (ImGui::Button("New World", button)) {
		ImGui::OpenPopup("New World##kit");
	}
	if (ImGui::BeginPopupModal("New World##kit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted((GameText::line(GameText::Original{45, 0xe4e4784b228d4e31ull}, "Every creature in this world will be lost.") + " Are you sure?").c_str());
		if (ImGui::Button("OK", ImVec2(100, 0))) {
			// a fresh copy of Eden, saved under the current world's name
			GameUI::request_world_load("Eden.sfc", name);
			s_backup_message = "A new world has begun.";
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (ImGui::Button("More Eggs!", button)) {
		if (s_eggs.empty()) {
			load_egg_record();
		}
		if (s_eggs != "xxxxxx") {
			s_backup_message = GameText::line(GameText::Original{62, 0xac144b4590419f89ull}, "This is only for when every last egg has gone.");
		} else {
			ImGui::OpenPopup("More Eggs!");
		}
	}
	if (ImGui::BeginPopupModal("More Eggs!", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(GameText::line(GameText::Original{55, 0x0f0b492f9516ff4cull}, "Six new first-generation eggs").c_str());
		ImGui::TextUnformatted(GameText::line(GameText::Original{47, 0x842b35bd1a01df88ull}, "go into the Hatchery for you.").c_str());
		if (ImGui::Button("OK", ImVec2(100, 0))) {
			s_eggs.clear();
			std::ofstream(pref_file("hatchery.txt")) << "empty\n";
			load_egg_record();
			s_backup_message = "There are six new eggs in the Hatchery.";
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (ImGui::Button("Replenish Stocks", button)) {
		ImGui::OpenPopup("Replenish Stocks");
	}
	if (ImGui::BeginPopupModal("Replenish Stocks", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(GameText::line(GameText::Original{37, 0x042ad0cee34c0987ull}, "OK refills the herbs on the Health Kit's").c_str());
		ImGui::TextUnformatted(GameText::line(GameText::Original{38, 0xbc262144ac49eedaull}, "Doctor's page, and the aphrodisiacs").c_str());
		ImGui::TextUnformatted(GameText::line(GameText::Original{41, 0xd1c187a48997e7adull}, "in the Breeder's Kit.").c_str());
		if (ImGui::Button("OK", ImVec2(100, 0))) {
			const bool ok = KitStock::replenish("Health") && KitStock::replenish("Aphro");
			s_backup_message = ok ? "The herbs are back in stock." : "Couldn't find HealthBU or AphroBU in the game folder.";
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (!s_backup_message.empty()) {
		ImGui::Spacing();
		ImGui::TextUnformatted(s_backup_message.c_str());
	}
}

void draw_injector_kit() {
	if (!ImGui::BeginTabBar("injector")) {
		return;
	}
	cover_tab("Planet.bmp");
	if (begin_page("Inject")) {
		draw_add_object();
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

void draw_kit(Kit kit) {
	switch (kit) {
		case KIT_OWNERS: draw_owners_kit(); break;
		case KIT_HEALTH: draw_health_kit(); break;
		case KIT_SCIENCE: draw_science_kit(); break;
		case KIT_BREEDERS: draw_breeders_kit(); break;
		case KIT_HATCHERY: draw_hatchery(); break;
		case KIT_INJECTOR: draw_injector_kit(); break;
		case KIT_FUNERAL: draw_funeral_kit(); break;
		case KIT_SCORE: draw_score_kit(); break;
		case KIT_OBSERVATION: draw_observation_kit(); break;
		case KIT_BACKUP: draw_backup_kit(); break;
		case KIT_COUNT: break;
	}
    if (!s_kit_error.empty()) ImGui::TextWrapped("%s", s_kit_error.c_str());
}

} // namespace

void draw_menu_items() {
	for (size_t k = 0; k < KIT_COUNT; ++k) {
		ImGui::MenuItem(KITS[k].name, nullptr, &s_open[k], Wolfling::kit_allowed(KITS[k].button));
	}
}

void draw_toolbar_buttons() {
	for (size_t k = 0; k < KIT_COUNT; ++k) {
		if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(KITS[k].button).x + ImGui::GetStyle().FramePadding.x * 2) {
			ImGui::NewLine();
		}
        const bool was_open = s_open[k];
		if (was_open) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		}
		if (ImGui::Button(KITS[k].button)) {
			s_open[k] = !s_open[k];
		}
		if (was_open) {
			ImGui::PopStyleColor();
		}
		ImGui::SetItemTooltip("%s", KITS[k].name);
		ImGui::SameLine();
	}
}

void draw_panes();

void draw_windows() {
    ++s_check_frame;
    const auto settings = pref_file("kits.txt");
    if (s_preferences_path != settings) {
        s_preferences.load(settings);
        s_preferences_path = settings;
        s_restore_page.fill(true);
        s_observation.life_force_warning = std::clamp(s_preferences.get("observe.threshold", 25), 0, 100);
        s_observation.warn_life_force = s_preferences.get("observe.low", 1) != 0;
        s_observation.warn_pregnant = s_preferences.get("observe.pregnant", 1) != 0;
        s_observation.warn_birth = s_preferences.get("observe.birth", 1) != 0;
        s_observation.message_box = s_preferences.get("observe.message", 0) != 0;
    }
	record_deaths();
	// a strict or relaxed wolfling run allows only the kits that look
	for (size_t k = 0; k < KIT_COUNT; ++k) {
		if (s_open[k] && !Wolfling::kit_allowed(KITS[k].button)) {
			s_open[k] = false;
		}
	}
	struct EndFrame {
		~EndFrame() {
			if (UIStyle::classic()) {
				KitWindows::end_frame();
			} else {
				KitWindows::close_all();
			}
		}
	} end_frame;
	static std::array<bool, KIT_COUNT> was_open{};
	for (size_t k = 0; k < KIT_COUNT; ++k) {
		if (s_open[k] && !was_open[k] && k == KIT_HATCHERY) {
			s_eggs.clear(); // read the record again: an empty one (xxxxxx) gives six new eggs
		}
		if (!s_open[k] && was_open[k] && k == KIT_HATCHERY) {
			s_hatchery.sex_sound.stop();
			s_hatchery.sex_sound = {};
			s_hatchery.hovered = -1;
		}
        if (s_open[k] && !was_open[k]) s_restore_page[k] = true;
		was_open[k] = s_open[k];
        s_current_kit = k;
        s_page_index = 0;
		if (!s_open[k]) {
			continue;
		}
		// the Hatchery fits its picture; the other kits start at a comfortable size
		const bool fits_picture = k == KIT_HATCHERY;
		if (UIStyle::classic()) {
			// a window of its own, as the original's kits are separate programs (batch 6,
			// Q11), titled "Kit name - creature name"
			auto* subject = g_engine_context.objects->try_get(g_engine_context.world->current_norn);
			auto* c = subject ? subject->as_creature() : nullptr;
			std::string title = KITS[k].name;
			if (k != KIT_HATCHERY && k != KIT_BACKUP) {
				title += " - " + (c ? (c->history_name.empty() || c->history_name == "<Un Named>" ? c->moniker : c->history_name) : std::string());
			}
            bool on_top = s_preferences.get("top." + std::string(KITS[k].button), 0) != 0;
			// the Hatchery's 320 x 240 scene and the window's padding
			KitWindows::show(static_cast<int>(k), title, &s_open[k], on_top, fits_picture ? 336 : 0, fits_picture ? 320 : 0, [&] {
				draw_kit(static_cast<Kit>(k));
                if (!s_kit_error.empty()) ImGui::TextWrapped("%s", s_kit_error.c_str());
				if (!fits_picture) {
					ImGui::Separator();
					if (ImGui::Checkbox(GameText::line(GameText::Original{26, 0xb6d685cb9e6a5bacull}, "Keep this window on top").c_str(), &on_top))
                        save_preference("top." + std::string(KITS[k].button), on_top);
				}
			});
			continue;
		}
		if (s_tabbed) {
			continue; // drawn in the panel, below
		}
		if (!fits_picture) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(420, 320), ImVec2(FLT_MAX, FLT_MAX));
			ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
		}
		ImGui::SetNextWindowPos(ImVec2(40.f + 24.f * static_cast<float>(k), 40.f + 16.f * static_cast<float>(k)), ImGuiCond_FirstUseEver);
		if (ImGui::Begin(KITS[k].name, &s_open[k], fits_picture ? ImGuiWindowFlags_AlwaysAutoResize : 0)) {
            UIStyle::window_shadow();
            bool top = s_preferences.get("top." + std::string(KITS[k].button), 0) != 0;
            if (ImGui::Checkbox(GameText::line(GameText::Original{26, 0xb6d685cb9e6a5bacull}, "Keep this window on top").c_str(), &top)) save_preference("top." + std::string(KITS[k].button), top);
            if (top) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
			draw_kit(static_cast<Kit>(k));
		}
		ImGui::End();
	}
	draw_panes();
}

void set_panel(bool tabbed, float top, float bottom) {
	s_tabbed = tabbed;
	s_panel_top = top;
	s_panel_bottom = bottom;
}

// --- the modern layout's panes ---

namespace {

struct Section {
	const char* name;
	Kit kit; // the kit it comes from (for wolfling runs, which allow only some kits)
};
struct PaneInfo {
	const char* name;
	std::vector<Section> sections;
};
const PaneInfo& pane_info(Pane pane) {
	static const PaneInfo none{"", {}};
	static const PaneInfo norn{"Norn", {{"Overview", KIT_OWNERS}, {"Health", KIT_HEALTH}, {"Breeding", KIT_BREEDERS}, {"Album", KIT_OWNERS}, {"Registration", KIT_OWNERS}}};
	static const PaneInfo lab{"Lab", {{"Chemistry", KIT_SCIENCE}, {"Brain", KIT_SCIENCE}, {"Genes", KIT_SCIENCE}, {"Injections", KIT_SCIENCE}}};
	static const PaneInfo objects{"Objects", {{"Hatchery", KIT_HATCHERY}, {"Injector", KIT_INJECTOR}, {"Herbs", KIT_HEALTH}}};
	static const PaneInfo records{"Records", {{"Population", KIT_OBSERVATION}, {"Graveyard", KIT_FUNERAL}, {"World", KIT_SCORE}}};
	switch (pane) {
		case Pane::Norn: return norn;
		case Pane::Lab: return lab;
		case Pane::Objects: return objects;
		case Pane::Records: return records;
		default: return none;
	}
}

Pane s_pane = Pane::None;
int s_section = 0;
std::map<std::string, float> s_fit; // per section: the text scale that makes it fit

bool section_allowed(Pane pane, int section) {
	const auto& info = pane_info(pane);
	return section >= 0 && section < static_cast<int>(info.sections.size()) && Wolfling::kit_allowed(KITS[info.sections[static_cast<size_t>(section)].kit].button);
}

// where each kit's pages went (the Tools menu and Kits::toggle still open kits by name)
std::pair<Pane, int> pane_for_kit(size_t kit) {
	switch (kit) {
		case KIT_OWNERS: return {Pane::Norn, 0};
		case KIT_HEALTH: return {Pane::Norn, 1};
		case KIT_BREEDERS: return {Pane::Norn, 2};
		case KIT_SCIENCE: return {Pane::Lab, 0};
		case KIT_HATCHERY: return {Pane::Objects, 0};
		case KIT_INJECTOR: return {Pane::Objects, 1};
		case KIT_OBSERVATION: return {Pane::Records, 0};
		case KIT_FUNERAL: return {Pane::Records, 1};
		case KIT_SCORE:
		case KIT_BACKUP: return {Pane::Records, 2};
		default: return {Pane::None, 0};
	}
}

bool panes_on() {
	return s_tabbed && !UIStyle::classic();
}

// two columns when there's room, else one under the other
bool begin_columns(const char* id, int count, float min_width) {
	const float w = ImGui::GetContentRegionAvail().x;
	const int fit = std::max(1, std::min(count, static_cast<int>(w / std::max(1.f, min_width))));
	if (!ImGui::BeginTable(id, fit, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
		return false;
	}
	ImGui::TableNextColumn();
	return true;
}
void next_column() {
	ImGui::TableNextColumn();
}

void no_subject() {
	ImGui::Spacing();
	ImGui::TextUnformatted("There is no subject");
	ImGui::TextDisabled("Choose a Norn from the faces on the bar, or hatch one.");
}

void norn_overview(Creature& c) {
	const float k = UIStyle::scale();
	if (begin_columns("##overview", 2, 300 * k)) {
		ImGui::BeginGroup();
		UIImages::creature_portrait(c, 96 * k);
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::PushFont(UIStyle::title_font());
		ImGui::TextUnformatted(creature_name(c).c_str());
		ImGui::PopFont();
		const int generation = Family::generation_of(c.moniker);
		ImGui::TextDisabled("%s \xc2\xb7 %s%s", sex_name(c), c.dead ? "Dead" : life_stage_name(c.age), generation > 0 ? fmt::format(" \xc2\xb7 G{}", generation).c_str() : "");
		ImGui::TextDisabled("Age %s \xc2\xb7 %s", game_time(c.age_ticks).c_str(), c.moniker.c_str());
		if (ImGui::SmallButton("Family tree")) {
			Family::open_tree();
		}
		ImGui::EndGroup();
		ImGui::EndGroup();
		ImGui::Spacing();
		ImGui::TextDisabled("Mother");
		ImGui::SameLine(80 * k);
		ImGui::TextUnformatted(parent_label(c.history_moms_moniker).c_str());
		ImGui::TextDisabled("Father");
		ImGui::SameLine(80 * k);
		ImGui::TextUnformatted(parent_label(c.history_dads_moniker).c_str());
		if (!c.history_birthday.empty() || !c.history_birthplace.empty()) {
			ImGui::TextDisabled("Born");
			ImGui::SameLine(80 * k);
			ImGui::TextWrapped("%s%s%s", c.history_birthday.c_str(), c.history_birthplace.empty() ? "" : " at ", c.history_birthplace.c_str());
		}
		if (const auto* room = g_engine_context.map->find_room_containing(c.ground_x(), c.ground_y() - 1)) {
			ImGui::TextDisabled("Room");
			ImGui::SameLine(80 * k);
			ImGui::Text("(%d,%d)-(%d,%d), type %d", room->left, room->top, room->right, room->bottom, room->type);
		}
		// what it's up to now
		ImGui::Spacing();
		auto* it = g_engine_context.objects->try_get(c.attention);
		ImGui::TextDisabled("Now");
		ImGui::SameLine(80 * k);
		ImGui::TextWrapped("%s%s%s", c.dead ? "dead" : c.asleep ? "asleep" : decision_name(c.current_decision).c_str(), it && !c.asleep ? ", " : "", it && !c.asleep ? get_object_name(it).c_str() : "");
		// its latest photo
		std::vector<PhotoAlbum::Photo> photos;
		try { photos = PhotoAlbum::load(c.moniker); } catch (const std::exception&) {}
		if (!photos.empty()) {
			const auto& photo = photos.back();
			const float room_h = ImGui::GetContentRegionAvail().y - 8;
			const float sc = std::clamp(std::min((ImGui::GetContentRegionAvail().x - 4) / static_cast<float>(photo.width), room_h / static_cast<float>(photo.height)), 0.25f, 1.2f);
			if (static_cast<float>(photo.height) * sc > 40) {
				const auto* picture = UIImages::indexed_image(fmt::format("photo:{}:{}:{}", c.moniker, photos.size() - 1, photo.time), photo.width, photo.height, photo.pixels);
				const ImVec2 p = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(static_cast<float>(photo.width) * sc, static_cast<float>(photo.height) * sc));
				UIImages::draw_bitmap(ImGui::GetWindowDrawList(), picture, p, sc);
				ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + static_cast<float>(photo.width) * sc, p.y + static_cast<float>(photo.height) * sc), IM_COL32(186, 142, 64, 160), 2);
			}
		}
		next_column();
		const int32_t health = creature_health_percent(c);
		wide_meter(fmt::format("Life force {}%", health), static_cast<float>(health) / 100.f, UIStyle::health_colour(static_cast<float>(health) / 100.f));
		ImGui::TextDisabled("%s%s", c.dead ? "Dead" : c.asleep ? "Asleep" : "Awake", c.bacterium.state ? " \xc2\xb7 ill" : "");
		ImGui::SeparatorText("Strongest needs");
		std::vector<size_t> order(c.loci.drives.size());
		for (size_t i = 0; i < order.size(); ++i) {
			order[i] = i;
		}
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return c.loci.drives[a] > c.loci.drives[b]; });
		for (size_t i = 0; i < std::min<size_t>(8, order.size()); ++i) {
			const size_t d = order[i];
			wide_meter(Kits::chemical_name(d + 1), c.loci.drives[d] / 255.f, UIStyle::health_colour(1.f - c.loci.drives[d] / 255.f));
		}
		ImGui::SeparatorText("Temperature");
		chemical_bar(c, 4);
		chemical_bar(c, 5);
		ImGui::EndTable();
	}
}

void norn_health(Creature& c) {
	const float k = UIStyle::scale();
	if (begin_columns("##health", 2, 280 * k)) {
		ImGui::SeparatorText("Drives and needs");
		health_drives(c, ImGui::GetContentRegionAvail().x > 420 * k ? 2 : 1, 140 * k);
		next_column();
		ImGui::SeparatorText("Fitness");
		health_fitness(c, ImGui::GetContentRegionAvail().x);
		ImGui::SeparatorText("Brain activity");
		draw_lobe_activity(c);
		ImGui::EndTable();
	}
}

void norn_breeding(Creature& c) {
	const float k = UIStyle::scale();
	if (begin_columns("##breeding", 2, 300 * k)) {
		breeder_fertility(c);
		next_column();
		ImGui::SeparatorText("Aphrodisiacs");
		draw_shop("Aphro");
		ImGui::EndTable();
	}
}

void norn_album(Creature& c) {
	const float k = UIStyle::scale();
	if (begin_columns("##album", 2, 300 * k)) {
		PhotoAlbum::draw(&c, c.moniker, true);
		next_column();
		const float fit = std::clamp(std::min(ImGui::GetContentRegionAvail().x / 244.f, (ImGui::GetContentRegionAvail().y - 8) / 304.f), 0.4f, 1.4f);
		certificate(c, fit);
		ImGui::EndTable();
	}
}

void lab_chemistry(Creature& c) {
	science_chemistry(c, std::max(160.f, ImGui::GetContentRegionAvail().x - 8));
}

void lab_brain(Creature& c) {
	const float k = UIStyle::scale();
	if (begin_columns("##brain", 2, 280 * k)) {
		ImGui::SeparatorText("Decisions");
		draw_decisions(c);
		next_column();
		ImGui::SeparatorText("Brain scanner");
		draw_brain_scanner(c);
		ImGui::EndTable();
	}
}

void lab_genes(Creature& c) {
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	science_genes(c, ImVec2(avail.x, std::max(80.f, avail.y - ImGui::GetTextLineHeightWithSpacing() - 6)));
}

void lab_injections(Creature& c) {
	const float k = UIStyle::scale();
	if (begin_columns("##inject", 2, 260 * k)) {
		ImGui::SeparatorText("The Science Kit's injections");
		science_injections(c, false);
		next_column();
		ImGui::SeparatorText("Chemistry now");
		const auto& names = injection_mixtures();
		for (size_t m = 0; m < std::min<size_t>(names.size(), 8); ++m) {
			chemical_bar(c, 100 + m);
		}
		ImGui::EndTable();
	}
}

// the Object Injector's objects as cards: picture, name, how many are left, Add
void objects_injector() {
	const float k = UIStyle::scale();
	auto items = injector_items();
	if (items.empty()) {
		ImGui::TextDisabled("No add-on objects (.cob files) in the game folder.");
		return;
	}
	const float card_w = 150 * k, card_h = 150 * k;
	const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (card_w + 8)));
	ImGui::BeginChild("##cards", ImVec2(0, 0), ImGuiChildFlags_None);
	for (size_t i = 0; i < items.size(); ++i) {
		auto& item = items[i];
		if (i % static_cast<size_t>(columns) != 0) {
			ImGui::SameLine(0, 8);
		}
		ImGui::PushID(static_cast<int>(i));
		const ImVec2 p = ImGui::GetCursorScreenPos();
		auto* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(p, ImVec2(p.x + card_w, p.y + card_h), IM_COL32(60, 44, 30, 230), 8);
		draw->AddRect(p, ImVec2(p.x + card_w, p.y + card_h), IM_COL32(186, 142, 64, 140), 8, 0, 1);
		if (item.picture) {
			const float fit = std::min((card_w - 16) / static_cast<float>(item.picture->width), (card_h * 0.45f) / static_cast<float>(item.picture->height));
			const float sc = std::clamp(fit, 0.5f, 3.f);
			UIImages::draw_bitmap(draw, item.picture, ImVec2(p.x + (card_w - static_cast<float>(item.picture->width) * sc) / 2, p.y + 8), sc);
		}
		ImGui::SetCursorScreenPos(ImVec2(p.x + 8, p.y + card_h * 0.52f));
		ImGui::PushTextWrapPos(p.x + card_w - 8);
		ImGui::TextUnformatted(item.name.c_str());
		ImGui::PopTextWrapPos();
		ImGui::SetCursorScreenPos(ImVec2(p.x + 8, p.y + card_h - ImGui::GetFrameHeight() - 8));
		ImGui::BeginDisabled(item.remaining == 0);
		if (ImGui::Button("Add", ImVec2(card_w * 0.45f, 0))) {
			add_item(item);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("%s", item.remaining < 0 ? "" : fmt::format("{} left", item.remaining).c_str());
		ImGui::SetCursorScreenPos(p);
		ImGui::Dummy(ImVec2(card_w, card_h + 8));
		ImGui::PopID();
	}
	ImGui::EndChild();
}

void objects_herbs() {
	const float k = UIStyle::scale();
	if (begin_columns("##herbs", 2, 300 * k)) {
		ImGui::SeparatorText("Doctor's page");
		draw_shop("Health");
		next_column();
		ImGui::SeparatorText("Aphrodisiacs");
		draw_shop("Aphro");
		ImGui::EndTable();
	}
	if (ImGui::Button("Replenish the stocks")) {
		const bool ok = KitStock::replenish("Health") && KitStock::replenish("Aphro");
		s_backup_message = ok ? "The herbs are back in stock." : "Couldn't find HealthBU or AphroBU in the game folder.";
	}
}

void records_population() {
	const float k = UIStyle::scale();
	check_alerts();
	if (begin_columns("##population", 2, 340 * k)) {
		observation_details();
		next_column();
		ImGui::SeparatorText("Alerts");
		observation_options();
		ImGui::EndTable();
	}
}

void records_graveyard() {
	const float k = UIStyle::scale();
	if (begin_columns("##graveyard", 2, 380 * k)) {
		funeral_graves(std::max(120.f, ImGui::GetContentRegionAvail().y - 8));
		next_column();
		funeral_photos();
		ImGui::EndTable();
	}
}

void records_world() {
	const float k = UIStyle::scale();
	if (begin_columns("##world", 2, 260 * k)) {
		ImGui::SeparatorText("Score");
		score_page();
		next_column();
		ImGui::SeparatorText("Emergency");
		draw_backup_kit();
		ImGui::EndTable();
	}
}

void draw_section(Pane pane, int section) {
	Creature* c = nullptr;
	if (pane == Pane::Norn || pane == Pane::Lab) {
		c = subject();
		if (!c) {
			no_subject();
			return;
		}
		if (s_registration_loaded.insert(c->moniker).second) {
			try { load_registration(*c); } catch (const std::exception& e) { s_kit_error = e.what(); }
		}
		s_history.sample(*c, g_engine_context.world->tick);
	}
	switch (pane) {
		case Pane::Norn:
			switch (section) {
				case 0: norn_overview(*c); break;
				case 1: norn_health(*c); break;
				case 2: norn_breeding(*c); break;
				case 3: norn_album(*c); break;
				default: owner_details(*c, false); break;
			}
			break;
		case Pane::Lab:
			switch (section) {
				case 0: lab_chemistry(*c); break;
				case 1: lab_brain(*c); break;
				case 2: lab_genes(*c); break;
				default: lab_injections(*c); break;
			}
			break;
		case Pane::Objects:
			switch (section) {
				case 0: {
					const ImVec2 avail = ImGui::GetContentRegionAvail();
					draw_hatchery(std::clamp(std::min(avail.x / 320.f, (avail.y - ImGui::GetTextLineHeightWithSpacing() * 2) / 240.f), 0.5f, 3.f));
					break;
				}
				case 1: objects_injector(); break;
				default: objects_herbs(); break;
			}
			break;
		case Pane::Records:
			switch (section) {
				case 0: records_population(); break;
				case 1: records_graveyard(); break;
				default: records_world(); break;
			}
			break;
		default: break;
	}
	if (!s_kit_error.empty()) {
		ImGui::TextWrapped("%s", s_kit_error.c_str());
	}
}

} // namespace

void draw_panes() {
	if (!panes_on() || s_pane == Pane::None) {
		return;
	}
	const auto& info = pane_info(s_pane);
	if (!section_allowed(s_pane, s_section)) {
		// a wolfling run allows only some kits: the first section it allows, or nothing
		int first = -1;
		for (int i = 0; i < static_cast<int>(info.sections.size()) && first < 0; ++i) {
			if (section_allowed(s_pane, i)) first = i;
		}
		if (first < 0) {
			s_pane = Pane::None;
			return;
		}
		s_section = first;
	}
	// the kits' own bookkeeping (the Hatchery reloads its eggs when it opens, and so on)
	// follows the section on show
	for (size_t k = 0; k < KIT_COUNT; ++k) {
		s_open[k] = k == info.sections[static_cast<size_t>(s_section)].kit;
	}
	auto& io = ImGui::GetIO();
	const float k = UIStyle::scale();
	const float margin = 12 * k;
	const float width = std::clamp(io.DisplaySize.x * 0.56f, std::min(560 * k, io.DisplaySize.x - 2 * margin), std::min(1000 * k, io.DisplaySize.x - 2 * margin));
	const float height = std::max(260.f, io.DisplaySize.y - s_panel_top - s_panel_bottom - 2 * margin);
	const ImVec2 pos(io.DisplaySize.x - width - margin, s_panel_top + margin);
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	const float border = 14 * k;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(border + 8 * k, border + 10 * k));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
	if (ImGui::Begin("##pane", nullptr, flags)) {
		auto* draw = ImGui::GetWindowDrawList();
		UIArt::window_frame(draw, pos, ImVec2(pos.x + width, pos.y + height), border, info.name);
		// close, at the top right of the frame
		const ImVec2 close(pos.x + width - border * 1.6f, pos.y + border * 0.9f);
		const float cr = border * 0.75f;
		ImGui::SetCursorScreenPos(ImVec2(close.x - cr, close.y - cr));
		if (ImGui::InvisibleButton("##closepane", ImVec2(cr * 2, cr * 2))) {
			s_pane = Pane::None;
		}
		const bool hot = ImGui::IsItemHovered();
		ImGui::SetItemTooltip("Close (Esc)");
		UIArt::brass_ring(draw, close, cr, cr * 0.62f);
		draw->AddCircleFilled(close, cr * 0.62f, hot ? IM_COL32(150, 60, 40, 255) : IM_COL32(60, 36, 22, 255), 20);
		draw->AddLine(ImVec2(close.x - cr * 0.3f, close.y - cr * 0.3f), ImVec2(close.x + cr * 0.3f, close.y + cr * 0.3f), UIArt::INK, 2);
		draw->AddLine(ImVec2(close.x - cr * 0.3f, close.y + cr * 0.3f), ImVec2(close.x + cr * 0.3f, close.y - cr * 0.3f), UIArt::INK, 2);
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			s_pane = Pane::None;
		}

		// the sections, as brass tabs
		ImGui::SetCursorScreenPos(ImVec2(pos.x + border + 8 * k, pos.y + border + 10 * k));
		for (int i = 0; i < static_cast<int>(info.sections.size()); ++i) {
			const char* label = info.sections[static_cast<size_t>(i)].name;
			const bool allowed = section_allowed(s_pane, i);
			const bool selected = i == s_section;
			const ImVec2 ts = ImGui::CalcTextSize(label);
			const ImVec2 size(ts.x + 26 * k, ts.y + 12 * k);
			if (i > 0) {
				ImGui::SameLine(0, 6 * k);
			}
			const ImVec2 tp = ImGui::GetCursorScreenPos();
			ImGui::BeginDisabled(!allowed);
			if (ImGui::InvisibleButton(label, size) && allowed) {
				s_section = i;
			}
			const bool hovered = ImGui::IsItemHovered();
			ImGui::EndDisabled();
			if (selected) {
				UIArt::brass_plate(draw, tp, ImVec2(tp.x + size.x, tp.y + size.y), 8 * k);
			} else {
				draw->AddRectFilled(tp, ImVec2(tp.x + size.x, tp.y + size.y), hovered ? IM_COL32(96, 66, 40, 255) : IM_COL32(64, 44, 28, 255), 8 * k);
				draw->AddRect(tp, ImVec2(tp.x + size.x, tp.y + size.y), IM_COL32(186, 142, 64, 120), 8 * k);
			}
			draw->AddText(ImVec2(tp.x + 13 * k, tp.y + 6 * k), selected ? IM_COL32(56, 34, 10, 255) : allowed ? UIArt::INK : IM_COL32(150, 130, 110, 150), label);
		}
		ImGui::Spacing();

		// the section itself, its text a little smaller if that's what it takes to fit
		const std::string key = fmt::format("{}:{}", static_cast<int>(s_pane), s_section);
		float& fit = s_fit.emplace(key, 1.f).first->second;
		ImGui::BeginChild("##section", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
		ImGui::SetWindowFontScale(fit);
		const float available = ImGui::GetContentRegionAvail().y;
		s_current_kit = info.sections[static_cast<size_t>(s_section)].kit;
		s_page_index = 0;
		draw_section(s_pane, s_section);
		const float used = ImGui::GetCursorPosY();
		if (used > available + 4 && fit > 0.72f) {
			fit = std::max(0.72f, fit * 0.95f);
		} else if (used < available * 0.85f && fit < 1.f) {
			fit = std::min(1.f, fit * 1.02f);
		}
		ImGui::EndChild();
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

void open_pane(Pane pane, int section) {
	if (pane == Pane::None || (pane == s_pane && section == s_section)) {
		s_pane = Pane::None;
		return;
	}
	s_pane = pane;
	s_section = section;
}

Pane current_pane() {
	return panes_on() ? s_pane : Pane::None;
}

const char* pane_name(Pane pane) {
	return pane_info(pane).name;
}

// --- what the bar's trays put into the world ---

namespace {

const c1cobfile* cached_cob(const std::string& key) {
	static std::map<std::string, std::optional<c1cobfile>> s_cobs;
	static uint64_t generation = 0;
	if (generation != g_engine_context.generation()) {
		s_cobs.clear();
		generation = g_engine_context.generation();
	}
	auto it = s_cobs.find(key);
	if (it == s_cobs.end()) {
		std::optional<c1cobfile> cob;
		try {
			auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, key);
			if (!path.empty()) {
				cob = read_c1cobfile(path);
			}
		} catch (std::exception&) {
		}
		it = s_cobs.emplace(key, std::move(cob)).first;
	}
	return it->second ? &*it->second : nullptr;
}

} // namespace

std::vector<QuickItem> injector_items(bool objects_only) {
	std::vector<QuickItem> items;
	for (const auto& file : list_injectable_cobs()) {
		// removers (.rcb) take objects away: not something to give
		if (objects_only && to_ascii_lowercase(file.extension().string()) != ".cob") {
			continue;
		}
		const std::string key = file.string();
		QuickItem item;
		item.key = "cob:" + key;
		item.name = file.stem().string();
		if (const c1cobfile* cob = cached_cob(key)) {
			if (!cob->name.empty()) {
				item.name = cob->name;
			}
			const auto& pic = cob->picture;
			if (pic) {
				std::vector<uint8_t> pixels(pic.data.data(), pic.data.data() + static_cast<size_t>(pic.width) * static_cast<size_t>(pic.height));
				item.picture = UIImages::indexed_image("cob:" + key, pic.width, pic.height, pixels);
			}
		}
		try {
			item.remaining = cob_remaining(file);
		} catch (const std::exception&) {
			item.remaining = 0;
		}
		items.push_back(std::move(item));
	}
	return items;
}

std::vector<QuickItem> shop_items(bool breeding) {
	std::vector<QuickItem> items;
	const std::string file = breeding ? "Aphro" : "Health";
	auto stock = KitStock::load(file);
	for (size_t i = 0; i < stock.size(); ++i) {
		const auto& entry = stock[i];
		QuickItem item;
		item.key = fmt::format("shop:{}:{}", file, i);
		item.name = entry.name;
		item.description = entry.description;
		item.remaining = static_cast<int>(entry.stock);
		// stored bottom row first, as in .cob files
		std::vector<uint8_t> upright(entry.pixels.size());
		const size_t row = static_cast<size_t>(std::max(0, entry.width));
		for (size_t y = 0; row && y < static_cast<size_t>(entry.height); ++y) {
			std::copy_n(entry.pixels.begin() + static_cast<long>((static_cast<size_t>(entry.height) - 1 - y) * row), row, upright.begin() + static_cast<long>(y * row));
		}
		item.picture = UIImages::indexed_image(fmt::format("{}:{}", file, i), entry.width, entry.height, upright);
		items.push_back(std::move(item));
	}
	return items;
}

bool add_item(const QuickItem& item) {
	try {
		if (item.key.rfind("cob:", 0) == 0) {
			inject_cob(item.key.substr(4));
			return true;
		}
		if (item.key.rfind("shop:", 0) == 0) {
			const auto colon = item.key.find(':', 5);
			const std::string file = item.key.substr(5, colon - 5);
			const size_t index = static_cast<size_t>(std::atoi(item.key.c_str() + colon + 1));
			auto stock = KitStock::load(file);
			if (index >= stock.size() || stock[index].stock == 0) {
				return false;
			}
			// the script ends with EDIT, so the new object goes straight into the hand
			Macro m(stock[index].script + ",endm");
			m.ownr = m.targ = g_engine_context.pointer->m_pointer_tool;
			g_engine_context.macros->run_now(m, false);
			stock[index].stock--;
			KitStock::save(file, stock);
			return true;
		}
	} catch (const std::exception& e) {
		s_kit_error = e.what();
		fmt::print(stderr, "* Couldn't add {:?}: {}\n", item.key, e.what());
	}
	return false;
}

const std::vector<std::string>& injection_names() {
	return injection_mixtures();
}

void inject(Creature& creature, size_t mixture, int dose) {
	inject_mixture_into(creature, mixture, dose);
}

void toggle(const std::string& name) {
	if (panes_on()) {
		for (size_t k = 0; k < KIT_COUNT; ++k) {
			if (name == KITS[k].button && Wolfling::kit_allowed(name)) {
				auto [pane, section] = pane_for_kit(k);
				open_pane(pane, section);
			}
		}
		return;
	}
	for (size_t k = 0; k < KIT_COUNT; ++k) {
		if (name == KITS[k].button && (s_open[k] || Wolfling::kit_allowed(name))) {
			s_open[k] = !s_open[k];
		}
	}
}

bool is_open(const std::string& name) {
	if (panes_on()) {
		for (size_t k = 0; k < KIT_COUNT; ++k) {
			if (name == KITS[k].button) {
				auto [pane, section] = pane_for_kit(k);
				return s_pane == pane && s_section == section;
			}
		}
		return false;
	}
	for (size_t k = 0; k < KIT_COUNT; ++k) {
		if (name == KITS[k].button) {
			return s_open[k];
		}
	}
	return false;
}

std::string chemical_name(size_t chemical) {
	return chemical < chemical_names().size() ? chemical_names()[chemical] : "";
}

std::string decision_name(int32_t decision) {
	const auto& names = decision_names();
	return decision >= 0 && static_cast<size_t>(decision) < names.size() ? names[static_cast<size_t>(decision)] : "";
}

void open_for_testing(const std::string& names) {
	for (size_t k = 0; k < KIT_COUNT; ++k) {
		if (names == "all" || names.find(KITS[k].button) != std::string::npos) {
			s_open[k] = true;
		}
	}
}

void flush_records() {
    record_deaths();
    if (s_graves_dirty) throw Exception("Couldn't persist graveyard " + graveyard_path().string());
}

void world_changed() {
    s_hatchery.sex_sound.stop();
    s_hatchery = {};
    KitWindows::close_all();
    s_alerted.clear();
    s_alerts.clear();
    s_eggs.clear();
    s_shop_page.clear();
    s_registration_loaded.clear();
    s_grave_selection.clear();
    PhotoAlbum::world_changed();
    UIImages::world_changed();
    s_kit_error.clear();
    s_preferences_path.clear();
	s_history = {};
	// the new world's graveyard is read when it's next needed
	s_graves_loaded = false;
    s_graves_dirty = false;
	s_graves.clear();
	s_buried.clear();
}

} // namespace Kits
