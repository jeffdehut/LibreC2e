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
#include "../../third_party/librecreatures/C1CobAnalysis.h"
#include "../../third_party/librecreatures/C1GenomeEdits.h"
#include "CreatureSprites.h"
#include "fileformats/sprImage.h"
#include "common/io/FileReader.h"
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

bool begin_page(const char* label, bool select = false) {
    const std::string key = "page." + std::string(KITS[s_current_kit].button);
    const int saved = s_preferences.get(key, 0);
    const int page = s_page_index++;
    const bool checking = std::getenv("OPENC1_KIT_CHECK") && !session_profile().empty();
    const bool restore = checking ? (s_check_frame / 4) % 7 == page : s_restore_page[s_current_kit] && saved == page_id(label);
    const bool active = ImGui::BeginTabItem(label, nullptr, restore || select ? ImGuiTabItemFlags_SetSelected : 0);
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

const std::vector<std::string>& chemical_names();

// a chemical nothing uses: the game names those by their number, or "not_allocated"
bool chemical_unused(size_t n) {
	const auto& name = chemical_names()[n];
	return name.empty() || name == std::to_string(n) || name.rfind("not_allocated", 0) == 0;
}

const std::vector<std::string>& chemical_names() {
	static std::vector<std::string> names;
    static uint64_t generation = 0;
	if (names.empty() || generation != g_engine_context.generation()) {
        generation = g_engine_context.generation();
		// allchemicals.str names every chemical by its number; chemicals.str is only the named
		// ones, one after another, so its places aren't chemical numbers past the drives
		names = read_strings("allchemicals.str", true);
		if (names.size() < 256) {
			names = read_strings("chemicals.str", false);
		}
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

bool lc_look(); // the classic layout's kits in LibreCreatures' design (below)

void draw_shop(const std::string& file) {
	auto items = KitStock::load(file);
	if (items.empty()) {
		ImGui::TextDisabled("Couldn't read the file \"%s\" in the game folder.", file.c_str());
		return;
	}
	if (lc_look()) {
		// LibreCreatures' Refill Dispensary (the 1996 kits had no way to get more): every item
		// back up to the 12 the game ships with; one a player has more of is left alone
		constexpr uint16_t SHIPPED = 12;
		const bool low = std::any_of(items.begin(), items.end(), [](const auto& i) { return i.stock < SHIPPED; });
		ImGui::BeginDisabled(!low);
		if (ImGui::Button("Refill dispensary")) {
			for (auto& i : items) {
				i.stock = std::max(i.stock, SHIPPED);
			}
			try {
				KitStock::save(file, items);
			} catch (const std::exception& e) {
				s_kit_error = e.what();
			}
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Every item back up to %u", SHIPPED);
		ImGui::SameLine();
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
			if (chemical_unused(d + 1)) {
				continue; // the drive slots past the thirteen the game uses
			}
			ImGui::TableNextColumn();
			UIStyle::meter(Kits::chemical_name(d + 1).c_str(), c.loci.drives[d] / 255.f, UIStyle::health_colour(1.f - c.loci.drives[d] / 255.f), width);
		}
		ImGui::EndTable();
	}
}

// --- the LibreCreatures kits' designs (the classic layout's default look) ---
//
// Adapted from h4rm0n1c/LibreCreatures' rebuilt kits (src/kits/, GNU GPL 3; see
// third_party/librecreatures/NOTICE.md): the same pages without covers, each reading
// labelled, where the 1996 kits drew unlabelled pictures. The 1996 look (covers and the
// original pictures from the game's folder) is the other choice, in Preferences.

bool lc_look() {
	return s_preferences.get("skin.librecreatures", 1) != 0;
}

} // namespace

void kit_look_option() {
	bool lc = lc_look();
	if (ImGui::Checkbox("Kits in LibreCreatures' design (otherwise the 1996 pages and covers)", &lc)) {
		save_preference("skin.librecreatures", lc ? 1 : 0);
	}
}

namespace {

ImU32 lc_text() {
	return ImGui::GetColorU32(ImGuiCol_Text);
}

// the room a page leaves under it: the kit window's separator and "always on top" box
// Whether LibreCreatures' pages are being drawn in the modern layout's panes, which give them
// our wood-and-brass facelift; in the classic layout's kit windows they keep the 1996 grey.
bool s_lc_in_pane = false;

struct LcTheme {
	ImU32 bar_back, bar_frame, plot_back, grid, grid_text, empty_text, cell, cell_on;
	ImVec4 heading, note_heading, note;
	bool rounded; // pill-shaped bars
};

const LcTheme& lc_theme() {
	static const LcTheme classic{IM_COL32(255, 255, 255, 255), IM_COL32(150, 150, 150, 255), IM_COL32(255, 255, 255, 255),
		IM_COL32(225, 225, 225, 255), IM_COL32(170, 170, 170, 255), IM_COL32(140, 140, 140, 255), IM_COL32(220, 220, 220, 255),
		IM_COL32(255, 255, 255, 255), ImVec4(0.3f, 0.45f, 0.85f, 1), ImVec4(0.45f, 0.42f, 0.38f, 1), ImVec4(0.55f, 0.52f, 0.48f, 1), false};
	// on the panes' paper: brown ink, and wood and brass for bars and graphs
	static const LcTheme modern{IM_COL32(222, 204, 168, 255), IM_COL32(128, 92, 46, 255), IM_COL32(236, 225, 199, 255),
		IM_COL32(205, 184, 148, 255), IM_COL32(112, 88, 58, 255), IM_COL32(128, 104, 74, 255), IM_COL32(224, 206, 172, 255),
		IM_COL32(104, 72, 28, 255), ImVec4(0.44f, 0.24f, 0.04f, 1), ImVec4(0.38f, 0.26f, 0.12f, 1), ImVec4(0.36f, 0.29f, 0.21f, 1), true};
	return s_lc_in_pane ? modern : classic;
}

// the room a page leaves under it: the kit window's separator and "always on top" box (a pane
// has neither)
float lc_footer() {
	return s_lc_in_pane ? 0.f : ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 3;
}

// a bar's background, fill (0-1 of it) and frame, as the theme draws them
void lc_bar(ImDrawList* d, ImVec2 a, ImVec2 b, float fill, ImU32 colour) {
	const auto& t = lc_theme();
	const float r = t.rounded ? (b.y - a.y) / 2 : 0;
	d->AddRectFilled(a, b, t.bar_back, r);
	if (fill > 0) {
		d->AddRectFilled(a, ImVec2(a.x + std::max((b.x - a.x) * std::clamp(fill, 0.f, 1.f), r * 2), b.y), colour, r);
	}
	d->AddRect(a, b, t.bar_frame, r);
}

// a heading and a few words, where a page has nothing to show yet
void lc_note(const char* heading, const char* body) {
	ImGui::Spacing();
	ImGui::TextColored(lc_theme().note_heading, "%s", heading);
	ImGui::PushStyleColor(ImGuiCol_Text, lc_theme().note);
	ImGui::TextWrapped("%s", body);
	ImGui::PopStyleColor();
}

void lc_heading(const char* text) {
	ImGui::Spacing();
	ImGui::TextColored(lc_theme().heading, "%s", text);
}

// a labelled bar: the label on the left, the bar, the value, and a word about it
void lc_reading(const std::string& label, int value, int maximum, ImU32 colour, const std::string& note = "", const std::string& value_text = "") {
	const float w = ImGui::GetContentRegionAvail().x;
	const float row = std::max(20.f, ImGui::GetTextLineHeight() + 8);
	const float label_w = std::min(150.f, w / 3), note_w = std::min(150.f, w / 4);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	auto* d = ImGui::GetWindowDrawList();
	const float ty = p.y + (row - ImGui::GetTextLineHeight()) / 2;
	d->AddText(ImVec2(p.x + 12, ty), lc_text(), label.c_str());
	const ImVec2 a(p.x + 12 + label_w, p.y + 3), b(p.x + w - note_w - 44, p.y + row - 3);
	lc_bar(d, a, b, maximum > 0 ? static_cast<float>(value) / static_cast<float>(maximum) : 0, colour);
	const std::string number = value_text.empty() ? std::to_string(value) : value_text;
	d->AddText(ImVec2(b.x + 6, ty), lc_text(), number.c_str());
	if (!note.empty()) {
		d->AddText(ImVec2(p.x + w - note_w + 4, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), note.c_str());
	}
	ImGui::Dummy(ImVec2(w, row));
}

ImU32 lc_blend(ImU32 a, ImU32 b, int percent) {
	auto ch = [&](int shift) {
		const int x = static_cast<int>((a >> shift) & 0xFF), y = static_cast<int>((b >> shift) & 0xFF);
		return static_cast<ImU32>(x + (y - x) * percent / 100);
	};
	return (ch(0) << 0) | (ch(8) << 8) | (ch(16) << 16) | (0xFFu << 24);
}

// Fitness: the 1996 page showed a skeleton, a thermometer and a heart monitor with no numbers;
// here the readings are labelled bars under headings.
void lc_fitness(const Creature& c) {
	const int life = creature_health_percent(c);
	lc_heading("Overall");
	lc_reading("Life force", life, 100, life >= 60 ? IM_COL32(40, 170, 60, 255) : life >= 30 ? IM_COL32(230, 170, 0, 255) : IM_COL32(210, 40, 40, 255), "", fmt::format("{}%", life));
	const int32_t months = c.sex == 2 ? creature_gestation_stage(c) : 0;
	ImGui::Text("      Health: %s     Age: %s%s", c.dead ? "dead" : c.bacterium.state ? "ill" : "healthy", c.dead ? "Dead" : life_stage_name(c.age),
		months ? fmt::format("     Pregnant: month {} of 8", months).c_str() : "");
	lc_heading("Temperature");
	{
		// coldness pulls one way, hotness the other: a marker on a blue-to-red scale
		const int cold = chemical(c, 4), hot = chemical(c, 5);
		const float w = ImGui::GetContentRegionAvail().x;
		const float row = std::max(20.f, ImGui::GetTextLineHeight() + 8);
		const float label_w = std::min(150.f, w / 3), note_w = std::min(150.f, w / 4);
		const ImVec2 p = ImGui::GetCursorScreenPos();
		auto* d = ImGui::GetWindowDrawList();
		const ImVec2 a(p.x + 12 + label_w, p.y + 3), b(p.x + w - note_w - 44, p.y + row - 3);
		const float mid = (a.x + b.x) / 2;
		d->AddRectFilledMultiColor(a, ImVec2(mid, b.y), IM_COL32(60, 110, 255, 255), IM_COL32(235, 235, 235, 255), IM_COL32(235, 235, 235, 255), IM_COL32(60, 110, 255, 255));
		d->AddRectFilledMultiColor(ImVec2(mid, a.y), b, IM_COL32(235, 235, 235, 255), IM_COL32(240, 60, 40, 255), IM_COL32(240, 60, 40, 255), IM_COL32(235, 235, 235, 255));
		d->AddRect(a, b, lc_theme().bar_frame);
		const int balance = hot - cold;
		const float marker = mid + static_cast<float>(balance) * (b.x - a.x) / 2 / 255;
		d->AddRectFilled(ImVec2(marker - 2, a.y - 2), ImVec2(marker + 2, b.y + 2), IM_COL32(0, 0, 0, 255));
		const float ty = p.y + (row - ImGui::GetTextLineHeight()) / 2;
		d->AddText(ImVec2(p.x + 12, ty), lc_text(), "Cold  /  hot");
		d->AddText(ImVec2(p.x + w - note_w + 4, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), balance < 0 ? "Feels cold" : balance > 0 ? "Feels hot" : "Neither");
		ImGui::Dummy(ImVec2(w, row));
		lc_reading("Coldness", cold, 255, IM_COL32(60, 110, 255, 255));
		lc_reading("Hotness", hot, 255, IM_COL32(240, 60, 40, 255));
	}
	lc_heading("Breathing and energy");
	lc_reading("Carbon dioxide", chemical(c, 62), 255, IM_COL32(120, 120, 140, 255));
	lc_reading("Glycogen (stored)", chemical(c, 59), 255, IM_COL32(200, 130, 30, 255));
	lc_reading("Glucose (in use)", chemical(c, 58), 255, IM_COL32(230, 180, 40, 255));
	lc_reading("Starch (eaten)", chemical(c, 57), 255, IM_COL32(170, 140, 90, 255));
}

// Drives: the 1996 page had gauges for five of them; here every drive has one, with its
// level, and the strongest is marked. One column, or two when there's room.
void lc_drives(const Creature& c) {
	constexpr int FIRST = 1, COUNT = 13;
	int strongest = 0;
	for (int i = 1; i < COUNT; ++i) {
		if (chemical(c, static_cast<size_t>(FIRST + i)) > chemical(c, static_cast<size_t>(FIRST + strongest))) {
			strongest = i;
		}
	}
	const int top_level = chemical(c, static_cast<size_t>(FIRST + strongest));
	ImGui::TextColored(lc_theme().heading, "%s", top_level > 0 ? ("Feeling most: " + chemical_names()[static_cast<size_t>(FIRST + strongest)]).c_str() : "No drive is felt at all.");
	const float w = ImGui::GetContentRegionAvail().x;
	const int columns = w >= 560 ? 2 : 1;
	const int per_column = (COUNT + columns - 1) / columns;
	const float row = std::max(18.f, std::min(34.f, (ImGui::GetContentRegionAvail().y - lc_footer()) / static_cast<float>(per_column)));
	const float column_w = (w - 8) / static_cast<float>(columns);
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	auto* d = ImGui::GetWindowDrawList();
	const ImU32 green = IM_COL32(90, 170, 60, 255), yellow = IM_COL32(250, 225, 40, 255), red = IM_COL32(200, 40, 30, 255);
	for (int i = 0; i < COUNT; ++i) {
		const float x = origin.x + static_cast<float>(i / per_column) * column_w;
		const float y = origin.y + static_cast<float>(i % per_column) * row;
		const int level = chemical(c, static_cast<size_t>(FIRST + i));
		const float ty = y + (row - ImGui::GetTextLineHeight()) / 2;
		d->AddText(ImVec2(x, ty), i == strongest && level > 0 ? IM_COL32(200, 30, 30, 255) : lc_text(), chemical_names()[static_cast<size_t>(FIRST + i)].c_str());
		// the 1996 gauge: green, yellow, red, faint where it isn't filled
		const ImVec2 a(x + 120, y + 4), b(x + column_w - 48, y + row - 4);
		const float mid = (a.x + b.x) / 2;
		// (blended with the bar's background: pale in the 1996 grey, dim on wood)
		const ImU32 back = lc_theme().bar_back;
		const ImU32 g2 = lc_blend(green, back, 55), y2 = lc_blend(yellow, back, 55), r2 = lc_blend(red, back, 55);
		d->AddRectFilledMultiColor(a, ImVec2(mid, b.y), g2, y2, y2, g2);
		d->AddRectFilledMultiColor(ImVec2(mid, a.y), b, y2, r2, r2, y2);
		const float filled = a.x + (b.x - a.x) * static_cast<float>(std::clamp(level, 0, 255)) / 255;
		d->PushClipRect(a, ImVec2(filled, b.y), true);
		d->AddRectFilledMultiColor(a, ImVec2(mid, b.y), green, yellow, yellow, green);
		d->AddRectFilledMultiColor(ImVec2(mid, a.y), b, yellow, red, red, yellow);
		d->PopClipRect();
		d->AddRect(a, b, lc_theme().bar_frame);
		d->AddText(ImVec2(b.x + 6, ty), lc_text(), std::to_string(level).c_str());
	}
	ImGui::Dummy(ImVec2(w, row * static_cast<float>(per_column)));
}

// Brain activity: the 1996 page lit up a cartoon brain; here each lobe is a row with its
// name, what it does, and how much of it is active.
const char* lc_lobe_name(size_t lobe) {
	static const char* names[] = {"Perception", "Drive", "Stimulus source", "Verb", "Noun", "General sense", "Decision", "Attention", "Concept"};
	return lobe < std::size(names) ? names[lobe] : "Lobe";
}
const char* lc_lobe_description(size_t lobe) {
	static const char* what[] = {
		"Copies of the drive, sense, verb and noun inputs, for the concept lobe",
		"One neuron per drive: how much the creature feels each need",
		"Which kind of object the last stimulus came from",
		"Verbs the creature has heard",
		"Nouns: the kinds of object seen or heard of",
		"Senses: touch, sounds, nearby creatures and walls",
		"One neuron per action; the strongest one is what the creature does",
		"Which kind of object the creature is attending to",
		"Learned associations between perceptions, which drive decisions",
	};
	return lobe < std::size(what) ? what[lobe] : "";
}
ImU32 lc_lobe_colour(size_t lobe) {
	static const ImU32 colours[] = {IM_COL32(80, 160, 255, 255), IM_COL32(255, 90, 90, 255), IM_COL32(255, 170, 60, 255), IM_COL32(120, 220, 120, 255),
		IM_COL32(60, 200, 200, 255), IM_COL32(230, 120, 230, 255), IM_COL32(255, 230, 70, 255), IM_COL32(170, 140, 255, 255), lc_theme().bar_frame};
	return colours[lobe % std::size(colours)];
}

void lc_brain(const Creature& c) {
	if (!c.brain || c.brain->lobes.empty()) {
		ImGui::TextDisabled("No brain lobes");
		return;
	}
	const auto& brain = *c.brain;
	std::vector<int> share(brain.lobes.size(), 0);
	size_t first = 0;
	int busiest = -1, busiest_share = 0;
	for (size_t l = 0; l < brain.lobes.size(); ++l) {
		const auto& lobe = brain.lobes[l];
		int active = 0;
		for (size_t n = 0; n < lobe.num_neurons && first + n < brain.neurons.size(); ++n) {
			active += brain.neurons[first + n].output > 0 ? 1 : 0;
		}
		share[l] = lobe.num_neurons ? active * 100 / static_cast<int>(lobe.num_neurons) : 0;
		if (share[l] > busiest_share) {
			busiest_share = share[l];
			busiest = static_cast<int>(l);
		}
		first += lobe.num_neurons;
	}
	const float w = ImGui::GetContentRegionAvail().x;
	const float row = std::max(30.f, std::min(46.f, (ImGui::GetContentRegionAvail().y - lc_footer()) / static_cast<float>(brain.lobes.size())));
	auto* d = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	for (size_t l = 0; l < brain.lobes.size(); ++l) {
		const float y = origin.y + static_cast<float>(l) * row;
		const ImU32 colour = lc_lobe_colour(l);
		d->AddRectFilled(ImVec2(origin.x + 6, y + 4), ImVec2(origin.x + 16, y + row - 6), colour);
		d->AddText(ImVec2(origin.x + 22, y + 2), static_cast<int>(l) == busiest ? IM_COL32(200, 30, 30, 255) : lc_text(), lc_lobe_name(l));
		d->PushClipRect(ImVec2(origin.x, y), ImVec2(origin.x + w, y + row), true);
		d->AddText(ImVec2(origin.x + 22, y + row / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), lc_lobe_description(l));
		d->PopClipRect();
		const ImVec2 a(origin.x + w - 230, y + 4), b(origin.x + w - 60, y + row / 2 - 1);
		lc_bar(d, a, b, static_cast<float>(share[l]) / 100, lc_blend(colour, IM_COL32(0, 0, 0, 255), 20));
		d->AddText(ImVec2(b.x + 6, y + 2), lc_text(), fmt::format("{}%", share[l]).c_str());
	}
	ImGui::Dummy(ImVec2(w, row * static_cast<float>(brain.lobes.size())));
}

void draw_health_kit() {
	auto* c = require_subject();
	if (!c || !ImGui::BeginTabBar("health")) {
		return;
	}
	// tab order and labels as the original's (batch 5, Q7); LibreCreatures' pages have no cover
	const bool lc = lc_look();
	if (!lc) {
		cover_tab("Health.bmp");
	}
	if (begin_page("Fitness page")) {
		if (lc) {
			lc_fitness(*c);
		} else {
			health_fitness(*c);
		}
		ImGui::EndTabItem();
	}
	if (begin_page("Drives and needs")) {
		if (lc) {
			lc_drives(*c);
		} else {
			health_drives(*c);
		}
		ImGui::EndTabItem();
	}
	if (begin_page("Brain activity")) {
		if (lc) {
			lc_brain(*c);
		} else {
			draw_lobe_activity(*c);
		}
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

// LibreCreatures' Science Kit pages: one graph filling the page with any of the named
// chemicals ticked beside it; the whole brain as the grid it's laid out on, every lobe
// outlined and named; the decision lobe with its values; the genome as a list of
// properties; and injections whose dose is what's added.
std::vector<int> s_lc_tracked = {1, 2, 3, 4};
constexpr size_t LC_MAX_TRACKED = 16;

ImU32 lc_series_colour(size_t i) {
	static const ImU32 colours[] = {IM_COL32(220, 40, 40, 255), IM_COL32(40, 110, 220, 255), IM_COL32(40, 160, 60, 255), IM_COL32(200, 140, 0, 255),
		IM_COL32(150, 60, 200, 255), IM_COL32(0, 160, 170, 255), IM_COL32(230, 90, 160, 255), IM_COL32(110, 110, 110, 255),
		IM_COL32(140, 80, 30, 255), IM_COL32(90, 170, 230, 255), IM_COL32(150, 200, 40, 255), IM_COL32(250, 120, 60, 255),
		IM_COL32(60, 60, 160, 255), IM_COL32(200, 200, 60, 255), IM_COL32(20, 100, 80, 255), IM_COL32(180, 30, 100, 255)};
	return colours[i % std::size(colours)];
}

// a graph of chemicals over the last minutes, newest on the right, each in its own colour
void lc_chemical_graph(const std::vector<int>& chemicals, float graph_w, float h, const char* empty = "") {
	const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + graph_w, a.y + h);
	auto* d = ImGui::GetWindowDrawList();
	d->AddRectFilled(a, b, lc_theme().plot_back);
	for (int level = 64; level < 256; level += 64) {
		const float y = b.y - (b.y - a.y) * static_cast<float>(level) / 255;
		d->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), lc_theme().grid);
		d->AddText(ImVec2(a.x + 2, y - ImGui::GetTextLineHeight()), lc_theme().grid_text, std::to_string(level).c_str());
	}
	for (size_t i = 0; i < chemicals.size(); ++i) {
		const auto& samples = s_history.samples[static_cast<size_t>(chemicals[i])];
		if (samples.size() < 2) {
			continue;
		}
		const size_t shown = std::min<size_t>(samples.size(), static_cast<size_t>(std::max(2.f, graph_w)));
		std::vector<ImVec2> points;
		for (size_t k = samples.size() - shown; k < samples.size(); ++k) {
			const float x = b.x - (b.x - a.x) * static_cast<float>(samples.size() - 1 - k) / static_cast<float>(shown - 1);
			points.emplace_back(x, b.y - (b.y - a.y) * samples[k] / 255.f);
		}
		d->AddPolyline(points.data(), static_cast<int>(points.size()), lc_series_colour(i), 0, 1.5f);
	}
	d->AddRect(a, b, lc_theme().bar_frame);
	if (chemicals.empty() && *empty) {
		d->AddText(ImVec2(a.x + 8, a.y + 8), lc_theme().empty_text, empty);
	}
	ImGui::Dummy(ImVec2(graph_w, h));
}

// the chemicals a player has saved as sets, by name: lines of "name|1,2,3"
std::vector<std::pair<std::string, std::vector<int>>>& lc_saved_sets() {
	static std::vector<std::pair<std::string, std::vector<int>>> sets;
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		std::ifstream in(pref_file("chemical sets.txt"));
		for (std::string line; std::getline(in, line);) {
			const auto bar = line.find('|');
			if (bar == std::string::npos || bar == 0) {
				continue;
			}
			std::vector<int> chemicals;
			std::istringstream numbers(line.substr(bar + 1));
			for (std::string n; std::getline(numbers, n, ',');) {
				try {
					const int k = std::stoi(n);
					if (k > 0 && k < 256 && chemicals.size() < LC_MAX_TRACKED) {
						chemicals.push_back(k);
					}
				} catch (const std::exception&) {
				}
			}
			sets.emplace_back(line.substr(0, bar), chemicals);
		}
	}
	return sets;
}

void lc_save_sets() {
	std::string text;
	for (const auto& [name, chemicals] : lc_saved_sets()) {
		text += name + "|";
		for (size_t i = 0; i < chemicals.size(); ++i) {
			text += (i ? "," : "") + std::to_string(chemicals[i]);
		}
		text += "\n";
	}
	try {
		write_persistent_file(pref_file("chemical sets.txt"), text);
	} catch (const std::exception& e) {
		s_kit_error = e.what();
	}
}

void lc_biochemistry(const Creature& c) {
	static std::vector<Theme> themes;
	static uint64_t generation = ~0ull;
	if (generation != g_engine_context.generation()) {
		themes = read_themes();
		generation = g_engine_context.generation();
	}
	const float h = ImGui::GetContentRegionAvail().y - lc_footer();
	const float list_w = std::min(260.f, ImGui::GetContentRegionAvail().x * 0.42f);
	const float graph_w = ImGui::GetContentRegionAvail().x - list_w - 8;
	lc_chemical_graph(s_lc_tracked, graph_w, h, "Tick chemicals in the list, or pick a theme.");
	ImGui::SameLine();
	ImGui::BeginGroup();
	auto& sets = lc_saved_sets();
	ImGui::SetNextItemWidth(list_w);
	if (ImGui::BeginCombo("##theme", "Theme or saved set", ImGuiComboFlags_None)) {
		for (const auto& theme : themes) {
			if (ImGui::Selectable(theme.name.c_str())) {
				s_lc_tracked.assign(theme.chemicals.begin(), theme.chemicals.end());
				s_lc_tracked.erase(std::remove(s_lc_tracked.begin(), s_lc_tracked.end(), 0), s_lc_tracked.end());
			}
		}
		if (!sets.empty()) {
			ImGui::SeparatorText("Saved");
		}
		for (size_t i = 0; i < sets.size(); ++i) {
			if (ImGui::Selectable(fmt::format("{}##set{}", sets[i].first, i).c_str())) {
				s_lc_tracked = sets[i].second;
			}
			if (ImGui::BeginPopupContextItem()) {
				if (ImGui::MenuItem("Delete this set")) {
					sets.erase(sets.begin() + static_cast<std::ptrdiff_t>(i));
					lc_save_sets();
				}
				ImGui::EndPopup();
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("Right-click a saved set to delete it");
	if (ImGui::Button("Clear graph")) {
		s_lc_tracked.clear();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(s_lc_tracked.empty());
	if (ImGui::Button("Save set")) {
		ImGui::OpenPopup("##saveset");
	}
	ImGui::EndDisabled();
	if (ImGui::BeginPopup("##saveset")) {
		static char name[48] = "";
		ImGui::SetNextItemWidth(200);
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		const bool entered = ImGui::InputTextWithHint("##setname", "Name of the set", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue);
		if ((entered || ImGui::Button("Save")) && name[0]) {
			sets.erase(std::remove_if(sets.begin(), sets.end(), [&](const auto& set) { return set.first == name; }), sets.end());
			sets.emplace_back(name, s_lc_tracked);
			lc_save_sets();
			name[0] = 0;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%zu of %zu", s_lc_tracked.size(), LC_MAX_TRACKED);
	if (ImGui::BeginTable("##chemicals", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY, ImVec2(list_w, ImGui::GetContentRegionAvail().y - lc_footer()))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("No.", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("No. ").x + 20);
		ImGui::TableSetupColumn("Chemical", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Level").x);
		ImGui::TableHeadersRow();
		for (int n = 1; n < 256; ++n) {
			const auto& name = chemical_names()[static_cast<size_t>(n)];
			if (chemical_unused(static_cast<size_t>(n))) {
				continue;
			}
			const auto at = std::find(s_lc_tracked.begin(), s_lc_tracked.end(), n);
			bool ticked = at != s_lc_tracked.end();
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(n);
			if (ticked) {
				ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::ColorConvertU32ToFloat4(lc_series_colour(static_cast<size_t>(at - s_lc_tracked.begin()))));
			}
			ImGui::BeginDisabled(!ticked && s_lc_tracked.size() >= LC_MAX_TRACKED);
			if (ImGui::Checkbox("##on", &ticked)) {
				if (ticked) {
					s_lc_tracked.push_back(n);
				} else {
					s_lc_tracked.erase(std::find(s_lc_tracked.begin(), s_lc_tracked.end(), n));
				}
			}
			ImGui::EndDisabled();
			if (at != s_lc_tracked.end()) {
				ImGui::PopStyleColor();
			}
			ImGui::PopID();
			ImGui::SameLine();
			ImGui::Text("%d", n);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(name.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%d", chemical(c, static_cast<size_t>(n)));
		}
		ImGui::EndTable();
	}
	ImGui::EndGroup();
}

// what a neuron stands for, where its lobe has a meaning per neuron
std::string lc_neuron_meaning(size_t lobe, size_t neuron) {
	if (lobe == LOBE_DRIVE && neuron < 16) {
		return chemical_names()[1 + neuron];
	}
	if (lobe == LOBE_DECISION && neuron < decision_names().size()) {
		return decision_names()[neuron];
	}
	return {};
}

void lc_brain_map(const Creature& c) {
	if (!c.brain || c.brain->lobes.empty()) {
		ImGui::TextDisabled("No brain lobes");
		return;
	}
	const auto& brain = *c.brain;
	static int measure = 0;
	ImGui::SetNextItemWidth(220);
	ImGui::Combo("Shade by", &measure, "Firing strength\0State\0Susceptibility\0Short-term weight\0Long-term weight\0Strength\0");
	uint32_t grid_w = 1, grid_h = 1;
	for (const auto& lobe : brain.lobes) {
		grid_w = std::max(grid_w, lobe.x + std::max(1u, lobe.width));
		grid_h = std::max(grid_h, lobe.y + std::max(1u, lobe.height));
	}
	const float h = ImGui::GetContentRegionAvail().y - lc_footer();
	const float list_w = std::min(200.f, ImGui::GetContentRegionAvail().x * 0.3f);
	const float map_w = ImGui::GetContentRegionAvail().x - list_w - 8;
	const float cell = std::max(2.f, std::min(map_w / static_cast<float>(grid_w), h / static_cast<float>(grid_h)));
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##brainmap", ImVec2(cell * static_cast<float>(grid_w), cell * static_cast<float>(grid_h)));
	const bool hovering = ImGui::IsItemHovered();
	const ImVec2 mouse = ImGui::GetMousePos();
	auto* d = ImGui::GetWindowDrawList();
	d->AddRectFilled(origin, ImVec2(origin.x + cell * static_cast<float>(grid_w), origin.y + cell * static_cast<float>(grid_h)), IM_COL32(30, 30, 36, 255));
	std::vector<int> share(brain.lobes.size(), 0);
	size_t first = 0;
	for (size_t l = 0; l < brain.lobes.size(); ++l) {
		const auto& lobe = brain.lobes[l];
		const uint32_t w = std::max(1u, lobe.width);
		const ImU32 colour = lc_lobe_colour(l);
		int active = 0;
		for (uint32_t n = 0; n < lobe.num_neurons && first + n < brain.neurons.size(); ++n) {
			const auto& neuron = brain.neurons[first + n];
			active += neuron.output > 0 ? 1 : 0;
			const float x = origin.x + static_cast<float>(lobe.x + n % w) * cell, y = origin.y + static_cast<float>(lobe.y + n / w) * cell;
			const uint8_t v = monitor_neuron(neuron, static_cast<BrainMeasure>(measure));
			// the square root, so the quiet neurons, most of a brain, aren't all one colour
			const int strength = v ? static_cast<int>(25 + 75 * std::sqrt(static_cast<float>(v) / 255.f)) : 12;
			d->AddRectFilled(ImVec2(x, y), ImVec2(x + cell - (cell > 4 ? 1 : 0), y + cell - (cell > 4 ? 1 : 0)), lc_blend(IM_COL32(30, 30, 36, 255), colour, strength));
			if (hovering && mouse.x >= x && mouse.x < x + cell && mouse.y >= y && mouse.y < y + cell) {
				const std::string meaning = lc_neuron_meaning(l, n);
				ImGui::SetTooltip("%s lobe, neuron %u%s%s\nFiring strength %d, state %d", lc_lobe_name(l), n, meaning.empty() ? "" : ": ", meaning.c_str(), neuron.output, neuron.state);
			}
		}
		share[l] = lobe.num_neurons ? active * 100 / static_cast<int>(lobe.num_neurons) : 0;
		first += lobe.num_neurons;
		const ImVec2 a(origin.x + static_cast<float>(lobe.x) * cell, origin.y + static_cast<float>(lobe.y) * cell);
		const ImVec2 b(a.x + static_cast<float>(w) * cell, a.y + static_cast<float>(std::max(1u, lobe.height)) * cell);
		d->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), colour, 0, 0, 1.5f);
	}
	// the names last, so no lobe covers another's
	for (size_t l = 0; l < brain.lobes.size(); ++l) {
		const auto& lobe = brain.lobes[l];
		const float size = ImGui::GetFontSize() * 0.85f;
		const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, lc_lobe_name(l));
		// above the lobe, kept inside the map
		const ImVec2 at(std::min(origin.x + static_cast<float>(lobe.x) * cell + 2, origin.x + cell * static_cast<float>(grid_w) - extent.x - 2),
			std::max(origin.y, origin.y + static_cast<float>(lobe.y) * cell - size - 1));
		d->AddRectFilled(ImVec2(at.x - 2, at.y), ImVec2(at.x + extent.x + 2, at.y + extent.y), IM_COL32(30, 30, 36, 220), 2);
		d->AddText(nullptr, size, at, lc_lobe_colour(l), lc_lobe_name(l));
	}
	ImGui::SameLine();
	ImGui::BeginGroup();
	for (size_t l = 0; l < brain.lobes.size(); ++l) {
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float row = ImGui::GetTextLineHeightWithSpacing();
		d->AddRectFilled(ImVec2(p.x, p.y + 3), ImVec2(p.x + 8, p.y + row - 3), lc_lobe_colour(l));
		ImGui::Dummy(ImVec2(10, row));
		ImGui::SameLine();
		ImGui::Text("%s %d%%", lc_lobe_name(l), share[l]);
	}
	ImGui::TextDisabled("Point at a neuron to see it.");
	ImGui::EndGroup();
}

void lc_decisions(Creature& c) {
	auto* it = g_engine_context.objects->try_get(c.attention);
	ImGui::TextColored(lc_theme().heading, "Now: %s%s%s", c.asleep ? "asleep" : decision_name(c.current_decision).c_str(), it ? ", " : "", it ? get_object_name(it).c_str() : "");
	if (!c.brain || c.brain->lobes.size() <= LOBE_DECISION) {
		ImGui::TextDisabled("No decision lobe");
		return;
	}
	static int measure = 0;
	ImGui::SetNextItemWidth(220);
	ImGui::Combo("Show", &measure, "Output (firing strength)\0State\0Susceptibility\0Short-term weight\0Long-term weight\0Strength\0");
	const auto& brain = *c.brain;
	size_t first = 0;
	for (size_t l = 0; l < LOBE_DECISION; ++l) {
		first += brain.lobes[l].num_neurons;
	}
	const auto& lobe = brain.lobes[LOBE_DECISION];
	const auto& names = decision_names();
	ImGui::BeginChild("##decisions", ImVec2(0, ImGui::GetContentRegionAvail().y - lc_footer()));
	int strongest = -1, best = -1;
	for (uint32_t n = 0; n < lobe.num_neurons && first + n < brain.neurons.size(); ++n) {
		if (brain.neurons[first + n].output > best) {
			best = brain.neurons[first + n].output;
			strongest = static_cast<int>(n);
		}
	}
	for (uint32_t n = 0; n < lobe.num_neurons && n < names.size() && first + n < brain.neurons.size(); ++n) {
		const int v = monitor_neuron(brain.neurons[first + n], static_cast<BrainMeasure>(measure));
		const bool top = static_cast<int>(n) == strongest && best > 0;
		lc_reading(top ? names[n] + "  <" : names[n], v, 255, top ? IM_COL32(210, 40, 40, 255) : IM_COL32(90, 130, 220, 255));
	}
	lc_heading("Learning");
	lc_reading("Reward", chemical(c, 49), 255, IM_COL32(40, 170, 60, 255));
	lc_reading("Punishment", chemical(c, 50), 255, IM_COL32(210, 40, 40, 255));
	ImGui::EndChild();
}

void lc_genetics(const Creature& c) {
	if (!c.genome) {
		ImGui::TextDisabled("No genome");
		return;
	}
	if (!ImGui::BeginTable("##genome", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY,
			ImVec2(0, ImGui::GetContentRegionAvail().y - lc_footer()))) {
		return;
	}
	ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 200);
	ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
	const auto row = [](const char* property, const std::string& value, bool heading = false) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		if (heading) {
			ImGui::TextColored(lc_theme().heading, "%s", property);
		} else {
			ImGui::Indent(16);
			ImGui::TextUnformatted(property);
			ImGui::Unindent(16);
		}
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(value.c_str());
	};
	row("Creature", "", true);
	row("Name", creature_name(c));
	row("Moniker", c.moniker);
	row("Species", c.genus == 1 ? "Norn" : c.genus == 2 ? "Grendel" : fmt::format("genus {}", c.genus));
	row("Sex", sex_name(c));
	row("Age", c.dead ? "Dead" : life_stage_name(c.age));
	row("Mother", c.mother.empty() ? "(none)" : c.mother);
	row("Father", c.father.empty() ? "(none)" : c.father);
	std::vector<std::vector<uint8_t>> records;
	try {
		records = genome_gene_bytes(c.moniker);
	} catch (const std::exception&) {
	}
	size_t bytes = 4;
	int mutable_genes = 0, male_only = 0, female_only = 0, highest_generation = 0;
	for (const auto& r : records) {
		bytes += r.size();
		if (r.size() >= 10) {
			mutable_genes += (r[9] & 1) ? 1 : 0;
			male_only += (r[9] & 8) ? 1 : 0;
			female_only += (r[9] & 16) ? 1 : 0;
			highest_generation = std::max<int>(highest_generation, r[7]);
		}
	}
	row("Genome", "", true);
	row("Genome file", records.empty() ? "(could not be read)" : fmt::format("{}.gen, {} bytes", c.moniker, bytes));
	row("Genes", std::to_string(c.genome->genes.size()));
	row("Genes that can mutate", std::to_string(mutable_genes));
	row("Male-only genes", std::to_string(male_only));
	row("Female-only genes", std::to_string(female_only));
	row("Highest generation", std::to_string(highest_generation));
	std::map<std::string, int> kinds;
	for (const auto& g : c.genome->genes) {
		kinds[g->name()]++;
	}
	row("Genes by type", "", true);
	for (const auto& [kind, count] : kinds) {
		row(kind.c_str(), fmt::format("{} gene{}", count, count == 1 ? "" : "s"));
	}
	if (c.brain) {
		row("Brain", "", true);
		for (size_t l = 0; l < c.brain->lobes.size(); ++l) {
			const auto& lobe = c.brain->lobes[l];
			row(fmt::format("{} lobe", lc_lobe_name(l)).c_str(), fmt::format("{} x {} = {} neurons, at {},{}", lobe.width, lobe.height, lobe.num_neurons, lobe.x, lobe.y));
		}
	}
	ImGui::EndTable();
}

// a repeating injection (LibreCreatures' Biochemistry Kit): every so many seconds, so many
// times (0: until stopped), into the creature it started on
struct LcRepeat {
	std::string moniker;
	int chemical = 0, amount = 0, every = 10, times = 0, left = 0;
	double next = 0;
	bool on = false;
};
LcRepeat s_lc_repeat;

void lc_add_chemical(Creature& c, int chemical, int amount) {
	if (c.biochemistry && chemical > 0 && static_cast<size_t>(chemical) < c.biochemistry->chemicals.size()) {
		auto& value = c.biochemistry->chemicals[static_cast<size_t>(chemical)].concentration;
		value = static_cast<uint8_t>(std::min(255, value + amount));
	}
}

void lc_run_repeat() {
	auto& r = s_lc_repeat;
	if (r.on && !Wolfling::kit_allowed("Science")) {
		r.on = false; // a wolfling run leaves its creatures alone
	}
	if (!r.on || GameUI::paused() || ImGui::GetTime() < r.next) {
		return;
	}
	Creature* target = nullptr;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature(); c && c->moniker == r.moniker && !c->dead) {
			target = c;
		}
	}
	if (!target) {
		r.on = false;
		return;
	}
	lc_add_chemical(*target, r.chemical, r.amount);
	r.next = ImGui::GetTime() + r.every;
	if (r.times > 0 && --r.left <= 0) {
		r.on = false;
	}
}

void lc_injections(Creature& c) {
	const auto& mixtures = injection_mixtures();
	static int chosen = -1; // a chemical number
	static int amount = 16;
	static char filter[32] = "";
	const float h = ImGui::GetContentRegionAvail().y - lc_footer();
	const float list_w = std::max(200.f, ImGui::GetContentRegionAvail().x * 0.45f);
	// the medicines first, then every named chemical, both narrowed by the filter
	ImGui::BeginGroup();
	ImGui::SetNextItemWidth(list_w);
	ImGui::InputTextWithHint("##filter", "Find a chemical", filter, sizeof(filter));
	const std::string find = to_ascii_lowercase(std::string(filter));
	const auto matches = [&](const std::string& name) { return find.empty() || to_ascii_lowercase(name).find(find) != std::string::npos; };
	if (ImGui::BeginListBox("##medicines", ImVec2(list_w, h - ImGui::GetFrameHeightWithSpacing()))) {
		ImGui::SeparatorText("Medicines");
		for (int k = 0; k < static_cast<int>(mixtures.size()); ++k) {
			if (matches(mixtures[static_cast<size_t>(k)]) && ImGui::Selectable(fmt::format("{}##m{}", mixtures[static_cast<size_t>(k)], k).c_str(), chosen == 100 + k)) {
				chosen = 100 + k;
			}
		}
		ImGui::SeparatorText("Every chemical");
		for (int n = 1; n < 256; ++n) {
			const auto& name = chemical_names()[static_cast<size_t>(n)];
			if (chemical_unused(static_cast<size_t>(n)) || !matches(name)) {
				continue;
			}
			if (ImGui::Selectable(fmt::format("{} ({})##c{}", name, n, n).c_str(), chosen == n)) {
				chosen = n;
			}
		}
		ImGui::EndListBox();
	}
	ImGui::EndGroup();
	ImGui::SameLine();
	ImGui::BeginGroup();
	if (chosen <= 0) {
		lc_note("No chemical chosen", "Choose a medicine or a chemical from the list to give it to the creature.");
	} else {
		ImGui::TextColored(lc_theme().heading, "%s", chemical_names()[static_cast<size_t>(chosen)].c_str());
		ImGui::SetNextItemWidth(std::min(240.f, ImGui::GetContentRegionAvail().x));
		ImGui::SliderInt("##dose", &amount, 1, 255, "Dose: %d");
		const int now = chemical(c, static_cast<size_t>(chosen)), after = std::min(255, now + amount);
		lc_reading("Now", now, 255, IM_COL32(90, 130, 220, 255));
		lc_reading("After the dose", after, 255, IM_COL32(40, 170, 60, 255));
		if (ImGui::Button("Inject")) {
			lc_add_chemical(c, chosen, amount); // the dose is the amount added
		}
		lc_heading("Repeat");
		auto& r = s_lc_repeat;
		ImGui::SetNextItemWidth(120);
		ImGui::InputInt("seconds apart", &r.every);
		r.every = std::clamp(r.every, 1, 3600);
		ImGui::SetNextItemWidth(120);
		ImGui::InputInt("times (0 until stopped)", &r.times);
		r.times = std::clamp(r.times, 0, 9999);
		if (r.on) {
			ImGui::Text("Giving %s every %d s%s", chemical_names()[static_cast<size_t>(r.chemical)].c_str(), r.every, r.times ? fmt::format(", {} more", r.left).c_str() : "");
			if (ImGui::Button("Stop")) {
				r.on = false;
			}
		} else if (ImGui::Button("Start repeating")) {
			r = LcRepeat{c.moniker, chosen, amount, r.every, r.times, r.times, ImGui::GetTime(), true};
		}
	}
	ImGui::EndGroup();
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
	const bool lc = lc_look();
	if (!lc) {
		cover_tab("Science.bmp");
	}
	if (begin_page("Biochemistry")) {
		if (lc) {
			lc_biochemistry(*c);
		} else {
			science_chemistry(*c);
		}
		ImGui::EndTabItem();
	}
	if (begin_page("Genetics")) {
		if (lc) {
			lc_genetics(*c);
		} else {
			science_genes(*c);
		}
		ImGui::EndTabItem();
	}
	if (begin_page("Brain scanner")) {
		if (lc) {
			lc_brain_map(*c);
		} else {
			draw_brain_scanner(*c);
		}
		ImGui::EndTabItem();
	}
	if (begin_page("Decisions")) {
		if (lc) {
			lc_decisions(*c);
		} else {
			draw_decisions(*c);
		}
		ImGui::EndTabItem();
	}
	if (begin_page("Injections")) {
		if (lc) {
			lc_injections(*c);
		} else {
			science_injections(*c);
		}
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

// LibreCreatures' Fertility page: the sex hormones the 1996 page read, as labelled bars
// with a swatch each, the pregnancy, and all of them graphed over the last minutes.
void lc_fertility(const Creature& c) {
	const bool female = c.sex == 2;
	// a female's progesterone (66), oestrogen (63) and gonadotrophin (65), then sex drive and
	// glycogen; a male's testosterone (64) in place of the first two
	const std::vector<int> chemicals = female ? std::vector<int>{66, 63, 65, 13, 59} : std::vector<int>{64, 65, 13, 59};
	ImGui::TextColored(lc_theme().heading, "%s, %s", creature_name(c).c_str(), female ? "female" : "male");
	ImGui::SameLine(0, 24);
	ImGui::TextDisabled("age %s    life force %d%%", c.dead ? "Dead" : life_stage_name(c.age), creature_health_percent(c));
	if (!female) {
		ImGui::TextDisabled("Males cannot become pregnant.");
	} else if (const int32_t months = creature_gestation_stage(c)) {
		ImGui::TextColored(ImVec4(0.67f, 0, 0.35f, 1), "Pregnant: month %d of 8", months);
	} else {
		ImGui::TextDisabled("Not pregnant.");
	}
	for (size_t i = 0; i < chemicals.size(); ++i) {
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float row = std::max(20.f, ImGui::GetTextLineHeight() + 8);
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + row / 2 - 4), ImVec2(p.x + 8, p.y + row / 2 + 4), lc_series_colour(i));
		lc_reading(chemical_names()[static_cast<size_t>(chemicals[i])], chemical(c, static_cast<size_t>(chemicals[i])), 255, lc_series_colour(i));
	}
	const float h = ImGui::GetContentRegionAvail().y - lc_footer();
	if (h > 40) {
		lc_chemical_graph(chemicals, ImGui::GetContentRegionAvail().x, h);
	}
}

void draw_breeders_kit() {
	auto* c = require_subject();
	if (!c || !ImGui::BeginTabBar("breeder")) {
		return;
	}
	// tab order and labels as the original's; the pregnancy display is on the fertility page
	// (batch 5, Q7)
	const bool lc = lc_look();
	if (!lc) {
		cover_tab("Sex.bmp");
	}
	s_history.sample(*c, g_engine_context.world->tick);
	if (begin_page("Fertility page")) {
		if (lc) {
			lc_fertility(*c);
		} else {
			breeder_fertility(*c);
		}
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

void take_egg(size_t slot, bool female, const std::string& genome = "") {
	// Batch 4, Q12: the Hatchery sends the game this script. The egg (eggs.spr frames 8n to
	// 8n+7, class 2 5 2) appears by the incubator, its genome a child of mum<n+1> and
	// dad<n+1>, its sex code in OBV1, and the camera moves there. The Designer's eggs carry a
	// genome it made beforehand.
	const int32_t n = static_cast<int32_t>(slot);
	const std::string gene = genome.empty() ? fmt::format("new: gene tokn mum{} tokn dad{} obv0", n + 1, n + 1) : fmt::format("setv obv0 tokn {}", genome);
	Macro m(fmt::format(
		"inst,sys: wtop,sys: cmra 2223 724,new: simp eggs 8 {} 2000 0,pose 3,setv clas 33882624,setv attr 67,"
		"{},setv obv1 {},tick 2400,dde: hatc,mvto {} 870,endm",
		8 * n, gene, female ? 2 : 1, 2408 + 40 * n));
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
		ImGui::SetTooltip("%s \xc2\xb7 double-click to hatch", female_egg ? "Female" : "Male");
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
		// the eggs left, as eggs (the ones taken, outlines), and how to use them
		const float h = ImGui::GetTextLineHeight();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		auto* eggs = ImGui::GetWindowDrawList();
		const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
		for (size_t i = 0; i < s_eggs.size(); ++i) {
			const ImVec2 at(p.x + h * 0.4f + static_cast<float>(i) * h * 0.8f, p.y + h / 2);
			if (s_eggs[i] == 'x') {
				eggs->AddEllipse(at, ImVec2(h * 0.26f, h * 0.36f), (ink & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 110), 0, 16, 1.2f);
			} else {
				eggs->AddEllipseFilled(at, ImVec2(h * 0.26f, h * 0.36f), ink, 0, 16);
			}
		}
		ImGui::Dummy(ImVec2(static_cast<float>(s_eggs.size()) * h * 0.8f, h));
		ImGui::SetItemTooltip("%ld egg%s left", static_cast<long>(left), left == 1 ? "" : "s");
		ImGui::SameLine();
		ImGui::TextDisabled("Point at an egg: male or female? Double-click to hatch it.");
	}
}

// LibreCreatures' Hatchery: the Nest (the 1996 picture, whose eggs are the mum<n> x dad<n>
// pairs) and the Designer, which makes an egg from any mother and father among mum1-mum9 and
// dad1-dad9, with a chosen sex, colour and genetic changes.
struct LcDesigner {
	int mum = 1, dad = 1, sex = 0; // sex 0 either, 1 male, 2 female
	bool colour = false;
	openc1::librecreatures::EggColours colours;
	openc1::librecreatures::DesignerChoices choices;
	std::string status;
	bool loaded = false;
};
LcDesigner s_designer;

void lc_designer_load() {
	auto& d = s_designer;
	if (d.loaded) {
		return;
	}
	d.loaded = true;
	d.mum = s_preferences.get("designer.mum", 1);
	d.dad = s_preferences.get("designer.dad", 1);
	d.sex = s_preferences.get("designer.sex", 0);
	d.colour = s_preferences.get("designer.colour", 0) != 0;
	for (int i = 0; i < 3; ++i) {
		d.colours.tint[i] = static_cast<uint8_t>(s_preferences.get(fmt::format("designer.tint{}", i), 0x80));
	}
	d.colours.hue_rotation = static_cast<uint8_t>(s_preferences.get("designer.hue", 0x80));
	d.colours.colour_swap = static_cast<uint8_t>(s_preferences.get("designer.swap", 0x80));
	d.choices.eat_better = s_preferences.get("designer.eat", 0) != 0;
	d.choices.no_old_age_death = s_preferences.get("designer.old", 0) != 0;
	d.choices.lock_learning = s_preferences.get("designer.learning", 0) != 0;
	d.choices.hardier_babies = s_preferences.get("designer.hardier", 0) != 0;
}

void lc_designer_save() {
	const auto& d = s_designer;
	save_preference("designer.mum", d.mum);
	save_preference("designer.dad", d.dad);
	save_preference("designer.sex", d.sex);
	save_preference("designer.colour", d.colour ? 1 : 0);
	for (int i = 0; i < 3; ++i) {
		save_preference(fmt::format("designer.tint{}", i), d.colours.tint[i]);
	}
	save_preference("designer.hue", d.colours.hue_rotation);
	save_preference("designer.swap", d.colours.colour_swap);
	save_preference("designer.eat", d.choices.eat_better ? 1 : 0);
	save_preference("designer.old", d.choices.no_old_age_death ? 1 : 0);
	save_preference("designer.learning", d.choices.lock_learning ? 1 : 0);
	save_preference("designer.hardier", d.choices.hardier_babies ? 1 : 0);
}

// a parent genome's face: its head sprite looking at the camera, as an adult; null if the
// genome or its sprite isn't there
const UIImages::Bitmap* lc_parent_face(const std::string& parent, bool female) {
	static std::map<std::string, std::optional<std::pair<std::string, Image>>> faces; // by parent, per world
	static uint64_t generation = ~0ull;
	if (generation != g_engine_context.generation()) {
		faces.clear();
		generation = g_engine_context.generation();
	}
	auto it = faces.find(parent);
	if (it == faces.end()) {
		std::optional<std::pair<std::string, Image>> face;
		try {
			if (auto genome = load_genome(parent)) {
				const auto breeds = appearance_breeds(*genome, female ? 2 : 1);
				std::string name;
				for (int breed = std::max(0, breeds[0]); breed >= 0 && name.empty(); --breed) {
					name = creature_body_part_sprite('A', female ? 4 : 0, 4, breed);
				}
				if (!name.empty()) {
					FileReader in(g_engine_context.paths->find_path(PATH_TYPE_IMAGE, name + ".spr"));
					auto frames = ReadSprFile(in);
					if (!frames.empty()) {
						face.emplace(name, frames[std::min<size_t>(8, frames.size() - 1)]);
					}
				}
			}
		} catch (const std::exception&) {
		}
		it = faces.emplace(parent, std::move(face)).first;
	}
	if (!it->second) {
		return nullptr;
	}
	const Image& image = it->second->second;
	std::vector<uint8_t> pixels(image.data.data(), image.data.data() + static_cast<size_t>(image.width) * static_cast<size_t>(image.height));
	return UIImages::indexed_image("face:" + it->second->first, image.width, image.height, pixels);
}

// a row of parents to choose from by their faces: mum1-mum9 or dad1-dad9, those that exist
bool lc_parent_row(const char* label, const char* prefix, bool female, int& chosen) {
	bool changed = false;
	ImGui::TextUnformatted(label);
	const float cell = std::clamp((ImGui::GetContentRegionAvail().x - 8) / 9, 36.f, 56.f);
	int shown = 0;
	for (int n = 1; n <= 9; ++n) {
		const std::string parent = fmt::format("{}{}", prefix, n);
		if (g_engine_context.paths->find_path(PATH_TYPE_GENETICS, parent + ".gen").empty()) {
			continue;
		}
		if (shown++) {
			ImGui::SameLine();
		}
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		if (ImGui::InvisibleButton(parent.c_str(), ImVec2(cell, cell))) {
			chosen = n;
			changed = true;
		}
		ImGui::SetItemTooltip("%s", parent.c_str());
		auto* d = ImGui::GetWindowDrawList();
		const bool on = chosen == n;
		d->AddRectFilled(pos, ImVec2(pos.x + cell, pos.y + cell), on ? lc_theme().cell_on : lc_theme().cell, 4);
		if (const auto* face = lc_parent_face(parent, female)) {
			const float scale = std::min((cell - 8) / static_cast<float>(face->width), (cell - 8) / static_cast<float>(face->height));
			UIImages::draw_bitmap(d, face, ImVec2(pos.x + (cell - static_cast<float>(face->width) * scale) / 2, pos.y + (cell - static_cast<float>(face->height) * scale) / 2), scale);
		}
		d->AddText(ImVec2(pos.x + 3, pos.y + cell - ImGui::GetTextLineHeight() - 1), lc_theme().grid_text, std::to_string(n).c_str());
		d->AddRect(pos, ImVec2(pos.x + cell, pos.y + cell), on ? IM_COL32(40, 90, 220, 255) : lc_theme().bar_frame, 4, 0, on ? 3.f : 1.f);
	}
	if (!shown) {
		ImGui::TextDisabled("No %s1.gen to %s9.gen in the Genetics folder.", prefix, prefix);
	}
	return changed;
}

// makes the designed egg: false, with the reason, if it can't
bool lc_make_designed_egg(std::string& message) {
	namespace lc = openc1::librecreatures;
	auto& d = s_designer;
	if (s_eggs.empty()) {
		load_egg_record();
	}
	const auto slot = s_eggs.find_first_not_of('x');
	if (slot == std::string::npos) {
		message = "The nest is empty: refill it on the Nest page first.";
		return false;
	}
	const std::string child = make_child_genome(fmt::format("mum{}", d.mum), fmt::format("dad{}", d.dad));
	if (child.empty()) {
		message = fmt::format("Couldn't find mum{}.gen and dad{}.gen in the Genetics folder.", d.mum, d.dad);
		return false;
	}
	auto genes = genome_gene_bytes(child);
	std::string summary;
	if (d.colour) {
		lc::append_colour_genes(genes, d.colours);
		summary = "coloured";
	}
	const std::string edits = lc::apply_designer_choices(genes, d.choices);
	if (!edits.empty()) {
		summary += (summary.empty() ? "" : "; ") + edits;
	}
	if (!summary.empty()) {
		save_genome_bytes(child, genes);
	}
	const bool female = d.sex == 0 ? rand_int32(0, 1) == 1 : d.sex == 2;
	play_kit_sound("hslt");
	take_egg(slot, female, child);
	s_eggs[slot] = 'x';
	save_egg_record();
	message = fmt::format("A {} egg (mum{} x dad{}) is in the incubator{}{}.", female ? "female" : "male", d.mum, d.dad, summary.empty() ? "" : ": ", summary);
	return true;
}

void lc_designer_page() {
	auto& d = s_designer;
	lc_designer_load();
	ImGui::BeginChild("##designer", ImVec2(0, ImGui::GetContentRegionAvail().y - lc_footer()));
	bool changed = lc_parent_row("Mother", "mum", true, d.mum);
	changed |= lc_parent_row("Father", "dad", false, d.dad);
	ImGui::Spacing();
	ImGui::SetNextItemWidth(140);
	changed |= ImGui::Combo("Sex", &d.sex, "Either\0Male\0Female\0");
	ImGui::SameLine(0, 24);
	changed |= ImGui::Checkbox("Colour this creature", &d.colour);
	if (d.colour) {
		const char* names[] = {"Red tint", "Green tint", "Blue tint", "Hue rotation", "Colour swap"};
		uint8_t* values[] = {&d.colours.tint[0], &d.colours.tint[1], &d.colours.tint[2], &d.colours.hue_rotation, &d.colours.colour_swap};
		ImGui::BeginGroup();
		for (int i = 0; i < 5; ++i) {
			int v = *values[i];
			ImGui::SetNextItemWidth(std::min(260.f, ImGui::GetContentRegionAvail().x - 200));
			if (ImGui::SliderInt(names[i], &v, 0, 255)) {
				*values[i] = static_cast<uint8_t>(v);
				changed = true;
			}
		}
		ImGui::EndGroup();
		ImGui::SameLine();
		ImGui::BeginGroup();
		// the tint, roughly: 128 in a channel is no change
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const auto channel = [](uint8_t t) { return static_cast<int>(std::clamp(128 + (static_cast<int>(t) - 128) * 2, 0, 255)); };
		ImGui::Dummy(ImVec2(56, 56));
		ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + 56, pos.y + 56), IM_COL32(channel(d.colours.tint[0]), channel(d.colours.tint[1]), channel(d.colours.tint[2]), 255), 4);
		ImGui::GetWindowDrawList()->AddRect(pos, ImVec2(pos.x + 56, pos.y + 56), IM_COL32(120, 120, 120, 255), 4);
		if (ImGui::Button("Randomise")) {
			for (auto& t : d.colours.tint) {
				t = static_cast<uint8_t>(rand_int32(0x20, 0xe0));
			}
			d.colours.hue_rotation = static_cast<uint8_t>(rand_int32(0, 255));
			d.colours.colour_swap = static_cast<uint8_t>(rand_int32(0, 255));
			changed = true;
		}
		ImGui::EndGroup();
	}
	ImGui::Spacing();
	ImGui::TextUnformatted("Genetics");
	changed |= ImGui::Checkbox("Eat better", &d.choices.eat_better);
	ImGui::SetItemTooltip("Rising hunger, tiredness, sleepiness, boredom, need for pleasure and sex drive no longer punish whatever the creature is doing, eating included");
	ImGui::SameLine(0, 24);
	changed |= ImGui::Checkbox("No death from old age", &d.choices.no_old_age_death);
	ImGui::SetItemTooltip("Removes the genes that end its life when it grows old");
	changed |= ImGui::Checkbox("Lock learning genes", &d.choices.lock_learning);
	ImGui::SetItemTooltip("Its brain and the chemistry it learns by can't mutate, so its offspring keep a brain that learns");
	ImGui::SameLine(0, 24);
	changed |= ImGui::Checkbox("Hardier babies", &d.choices.hardier_babies);
	ImGui::SetItemTooltip("More energy reserves at birth, and antibodies to every antigen");
	if (changed) {
		lc_designer_save();
	}
	ImGui::Spacing();
	if (ImGui::Button("Make this egg")) {
		lc_make_designed_egg(d.status);
	}
	if (!d.status.empty()) {
		ImGui::TextWrapped("%s", d.status.c_str());
	}
	ImGui::EndChild();
}

void draw_hatchery_kit() {
	if (!lc_look()) {
		draw_hatchery();
		return;
	}
	if (!ImGui::BeginTabBar("hatchery")) {
		return;
	}
	if (begin_page("Nest")) {
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (w - 320) / 2));
		ImGui::BeginGroup();
		draw_hatchery();
		ImGui::EndGroup();
		ImGui::EndTabItem();
	}
	if (begin_page("Designer")) {
		lc_designer_page();
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

// --- Funeral Kit ---

using Grave = GraveRecord;
std::vector<Grave> s_graves;
std::set<std::string> s_buried;
bool s_graves_loaded = false;
bool s_graves_writable = true;
bool s_graves_dirty = false;

// Each world has its own graveyard, in its folder (World.openc1.sfc ->
// Worlds/World.openc1/World.openc1.graveyard.txt). A world without one yet starts from the graveyard all worlds
// used to share, which is kept as it was.
std::filesystem::path graveyard_path() {
	return current_world_folder() / (std::filesystem::path(get_world_file_name()).stem().string() + ".graveyard.txt");
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
		g.parents = c.mother == c.father || c.father.empty() || parent(c.mother) == "the Hatchery" ? parent(c.mother) : parent(c.mother) + " and " + parent(c.father);
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
	for (const auto& moniker : death_row) {
		for (auto* obj : *g_engine_context.objects) {
			if (auto* c = obj->as_creature(); c && c->moniker == moniker) {
				bury(*c);
			}
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

// LibreCreatures' Funeral Kit: Deaths, a list of the dead (newest first) beside the chosen
// one's memorial, where the epitaph is written and the stone raised; and Graveyard, the
// raised stones to walk through.
int s_graveyard_stone = 0;
bool s_visit_graveyard = false;

void lc_memorial(Grave& g) {
	const bool unmarked = g.name.empty() || g.name == g.moniker;
	ImGui::TextColored(lc_theme().heading, "%s", unmarked ? "No Record" : g.name.c_str());
	ImGui::TextDisabled("%s, %s, lived %s", g.moniker.c_str(), g.sex.c_str(), g.age.c_str());
	if (!g.parents.empty()) {
		ImGui::TextDisabled("Child of %s", g.parents.c_str());
	}
	ImGui::TextDisabled("Born %s", g.born.empty() ? "(not recorded)" : g.born.c_str());
	if (!g.died.empty()) {
		ImGui::TextDisabled("Died %s", g.died.c_str());
	}
	ImGui::Spacing();
	ImGui::TextUnformatted("Epitaph");
	char text[128];
	std::snprintf(text, sizeof(text), "%s", g.headstone.c_str());
	ImGui::SetNextItemWidth(std::min(320.f, ImGui::GetContentRegionAvail().x));
	if (ImGui::InputText("##epitaph", text, sizeof(text))) {
		g.headstone = text;
		save_graves();
	}
	if (ImGui::Button(g.stone ? "Visit Headstone" : "Make Headstone")) {
		g.stone = true;
		save_graves();
		int index = 0;
		for (const auto& other : s_graves) {
			if (&other == &g) {
				break;
			}
			index += other.stone ? 1 : 0;
		}
		s_graveyard_stone = index;
		s_visit_graveyard = true;
	}
	ImGui::Spacing();
	// its photographs; the silhouette (Unknown.bmp) when nobody took any
	bool any = false;
	try {
		any = !PhotoAlbum::load(g.moniker).empty();
	} catch (const std::exception& e) {
		s_kit_error = e.what();
	}
	if (!any) {
		if (const auto* unknown = UIImages::bitmap("Unknown.bmp")) {
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(static_cast<float>(unknown->width), static_cast<float>(unknown->height)));
			UIImages::draw_bitmap(ImGui::GetWindowDrawList(), unknown, pos, 1);
		}
		ImGui::TextDisabled("No photographs were taken.");
		return;
	}
	const int32_t shown = PhotoAlbum::draw(nullptr, g.moniker, false);
	if (shown >= 0) {
		if (shown == g.photo) {
			ImGui::TextDisabled("This photograph is on the headstone.");
		} else if (ImGui::Button("Put this photograph on the headstone")) {
			g.photo = shown;
			save_graves();
		}
	}
}

void lc_deaths() {
	if (s_graves.empty()) {
		lc_note("No deaths yet",
			"When a creature dies, it is listed here, newest first. Select it to see its memorial: its photographs, "
			"its life span and an epitaph you can write. Make Headstone there raises its stone in the Graveyard.");
		return;
	}
	int chosen = chosen_grave();
	if (chosen < 0) {
		chosen = static_cast<int>(s_graves.size()) - 1;
		s_grave_selection = s_graves.back().moniker;
	}
	const float list_w = std::max(240.f, ImGui::GetContentRegionAvail().x * 0.45f);
	if (ImGui::BeginTable("##deaths", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY,
			ImVec2(list_w, ImGui::GetContentRegionAvail().y))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 30);
		ImGui::TableSetupColumn("Died", ImGuiTableColumnFlags_WidthStretch, 40);
		ImGui::TableSetupColumn("Stone", ImGuiTableColumnFlags_WidthStretch, 30);
		ImGui::TableHeadersRow();
		for (int i = static_cast<int>(s_graves.size()) - 1; i >= 0; --i) {
			const auto& g = s_graves[static_cast<size_t>(i)];
			const bool unmarked = g.name.empty() || g.name == g.moniker;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (ImGui::Selectable(fmt::format("{}##grave{}", unmarked ? g.moniker : g.name, i).c_str(), chosen == i, ImGuiSelectableFlags_SpanAllColumns)) {
				s_grave_selection = g.moniker;
				chosen = i;
			}
			ImGui::TableNextColumn();
			// "20 October 2026, 20:32": the day and month here, all of it on hover
			const auto month_end = g.died.find(' ', g.died.find(' ') + 1);
			ImGui::TextUnformatted(g.died.substr(0, month_end).c_str());
			if (month_end != std::string::npos) {
				ImGui::SetItemTooltip("%s", g.died.c_str());
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(unmarked ? "No record" : g.stone ? "Made" : "");
		}
		ImGui::EndTable();
	}
	ImGui::SameLine();
	ImGui::BeginChild("##memorial");
	lc_memorial(s_graves[static_cast<size_t>(chosen)]);
	ImGui::EndChild();
}

void lc_graveyard() {
	std::vector<size_t> stones;
	for (size_t i = 0; i < s_graves.size(); ++i) {
		if (s_graves[i].stone) {
			stones.push_back(i);
		}
	}
	if (stones.empty()) {
		lc_note("No headstones yet", s_graves.empty()
				? "No creature has died yet. When one does, it is listed on the Deaths tab, where you can write its epitaph and press Make Headstone to raise its stone here."
				: "Select a creature on the Deaths tab and press Make Headstone to raise its stone here.");
		return;
	}
	s_graveyard_stone = std::clamp(s_graveyard_stone, 0, static_cast<int>(stones.size()) - 1);
	auto& g = s_graves[stones[static_cast<size_t>(s_graveyard_stone)]];
	const auto* stone = UIImages::bitmap("Grave.bmp");
	const float w = stone ? static_cast<float>(stone->width) : 240;
	ImGui::SetCursorPosX(std::max(0.f, (ImGui::GetContentRegionAvail().x - w) / 2));
	ImGui::BeginGroup();
	draw_headstone(g);
	ImGui::BeginDisabled(s_graveyard_stone == 0);
	if (ImGui::ArrowButton("##stone_back", ImGuiDir_Left)) {
		--s_graveyard_stone;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::Text("%d of %d", s_graveyard_stone + 1, static_cast<int>(stones.size()));
	ImGui::SameLine();
	ImGui::BeginDisabled(s_graveyard_stone + 1 >= static_cast<int>(stones.size()));
	if (ImGui::ArrowButton("##stone_on", ImGuiDir_Right)) {
		++s_graveyard_stone;
	}
	ImGui::EndDisabled();
	ImGui::EndGroup();
	lc_note(stones.size() == 1 ? "1 headstone" : fmt::format("{} headstones", stones.size()).c_str(),
		stones.size() == 1 ? "Each stone you make on the Deaths tab stands here."
						   : "Use the arrows under the stone to walk the graveyard. Each stone you make on the Deaths tab stands here.");
}

void draw_funeral_kit() {
	if (!ImGui::BeginTabBar("funeral")) {
		return;
	}
	if (lc_look()) {
		if (begin_page("Deaths")) {
			lc_deaths();
			ImGui::EndTabItem();
		}
		if (begin_page("Graveyard", s_visit_graveyard)) {
			lc_graveyard();
			ImGui::EndTabItem();
		}
		s_visit_graveyard = false;
		ImGui::EndTabBar();
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
				// darker on the panes' paper, so it reads as well as the ink around it
				ImVec4 colour = UIStyle::health_colour(static_cast<float>(life) / 100.f);
				if (s_lc_in_pane) {
					colour = ImVec4(colour.x * 0.62f, colour.y * 0.62f, colour.z * 0.62f, 1);
				}
				ImGui::TextColored(colour, "%d%%", life);
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

// LibreCreatures' Ecology Kit, as the Observation Kit's Map page: the world's picture made
// small, its rooms coloured by what's chosen, every creature as a dot (Norns green, Grendels
// brown-red, others yellow, the dead grey) and every egg; clicking a dot chooses that creature
// and moves the view to it. (The engine has no weather, so where LibreCreatures colours by
// temperature this colours by kind of room.)
const UIImages::Bitmap* lc_world_picture() {
	// back.spr: 58 columns of 8 tiles, 144 x 150 each (frame = column * 8 + row), kept at a
	// sixth of the world's size
	constexpr int COLUMNS = 58, ROWS = 8, TILE_W = 144, TILE_H = 150, SHRINK = 6;
	constexpr int W = COLUMNS * TILE_W / SHRINK, H = ROWS * TILE_H / SHRINK;
	static std::vector<uint8_t> pixels;
	static uint64_t generation = ~0ull;
	if (generation != g_engine_context.generation()) {
		generation = g_engine_context.generation();
		pixels.clear();
		try {
			FileReader in(g_engine_context.paths->find_path(PATH_TYPE_IMAGE, "back.spr"));
			const auto tiles = ReadSprFile(in);
			if (tiles.size() >= static_cast<size_t>(COLUMNS * ROWS)) {
				pixels.assign(static_cast<size_t>(W * H), 0);
				for (int y = 0; y < H; ++y) {
					for (int x = 0; x < W; ++x) {
						const int wx = x * SHRINK, wy = y * SHRINK;
						const Image& tile = tiles[static_cast<size_t>((wx / TILE_W) * ROWS + wy / TILE_H)];
						const int tx = wx % TILE_W, ty = wy % TILE_H;
						if (tx < tile.width && ty < tile.height) {
							pixels[static_cast<size_t>(y * W + x)] = tile.data[static_cast<size_t>(ty * tile.width + tx)];
						}
					}
				}
			}
		} catch (const std::exception&) {
			pixels.clear();
		}
	}
	return pixels.empty() ? nullptr : UIImages::indexed_image("ecology:world", W, H, pixels);
}

void lc_world_map() {
	static int layer = s_preferences.get("ecology.layer", 0);
	ImGui::SetNextItemWidth(220);
	if (ImGui::Combo("Show", &layer, "Kinds of room\0Crowding\0Food and drink\0Toys and instruments\0Disease\0Rooms only\0")) {
		save_preference("ecology.layer", layer);
	}
	const auto& rooms = g_engine_context.map->get_rooms();
	const auto room_at = [&](float x, float y) -> int {
		for (size_t i = 0; i < rooms.size(); ++i) {
			const auto& r = rooms[i];
			if (x >= static_cast<float>(r.left) && x <= static_cast<float>(r.right) && y >= static_cast<float>(r.top) && y <= static_cast<float>(r.bottom)) {
				return static_cast<int>(i);
			}
		}
		return -1;
	};
	// what's in each room
	std::vector<int> counts(rooms.size(), 0);
	struct Dot {
		ImVec2 at;
		ImU32 colour;
		Creature* creature;
	};
	std::vector<Dot> dots;
	std::vector<ImVec2> eggs;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature()) {
			const ImVec2 feet(c->ground_x(), c->ground_y());
			dots.push_back({feet, c->dead ? lc_theme().bar_frame : c->genus == 1 ? IM_COL32(60, 230, 60, 255) : c->genus == 2 ? IM_COL32(200, 90, 40, 255) : IM_COL32(240, 220, 60, 255), c});
			const int room = room_at(feet.x, feet.y - 1);
			if (room >= 0 && !c->dead && (layer == 1 || (layer == 4 && c->bacterium.state))) {
				counts[static_cast<size_t>(room)]++;
			}
			continue;
		}
		const Rect2f box = obj->get_bbox();
		const ImVec2 bottom(box.x + box.width / 2, box.y + box.height - 1);
		if (obj->family == 2 && obj->genus == 5 && obj->species == 2) {
			if (bottom.y >= 0 && bottom.y < 1200) {
				eggs.push_back(bottom);
			}
			continue;
		}
		const bool food = obj->family == 2 && (obj->genus == 6 || obj->genus == 7);
		const bool toy = obj->family == 2 && (obj->genus == 13 || obj->genus == 14 || obj->genus == 9);
		if ((layer == 2 && food) || (layer == 3 && toy)) {
			const int room = room_at(bottom.x, bottom.y);
			if (room >= 0) {
				counts[static_cast<size_t>(room)]++;
			}
		}
	}
	const int largest = counts.empty() ? 0 : *std::max_element(counts.begin(), counts.end());
	// the map, as tall as the page allows, scrolling sideways when it's wider than the window
	const float legend_h = ImGui::GetTextLineHeightWithSpacing() * 3;
	const float avail_h = ImGui::GetContentRegionAvail().y - lc_footer() - legend_h;
	const float scale = std::max(ImGui::GetContentRegionAvail().x / static_cast<float>(CREATURES1_WORLD_WIDTH), std::min(avail_h - ImGui::GetStyle().ScrollbarSize, 400.f) / 1200.f);
	const float w = static_cast<float>(CREATURES1_WORLD_WIDTH) * scale, h = 1200 * scale;
	static bool placed = false;
	ImGui::BeginChild("##mapscroll", ImVec2(0, h + ImGui::GetStyle().ScrollbarSize + 2), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
	if (!placed) {
		// start where the view is
		ImGui::SetScrollX(std::max(0.f, (g_engine_context.viewport->window_x_to_world_x(ImGui::GetMainViewport()->Size.x / 2)) * scale - ImGui::GetWindowWidth() / 2));
		placed = true;
	}
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##worldmap", ImVec2(w, h));
	const bool clicked = ImGui::IsItemClicked();
	const bool hovering = ImGui::IsItemHovered();
	auto* d = ImGui::GetWindowDrawList();
	const auto at = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
	if (const auto* picture = lc_world_picture()) {
		UIImages::draw_bitmap(d, picture, origin, w / static_cast<float>(picture->width));
	} else {
		d->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + h), IM_COL32(40, 50, 60, 255));
	}
	for (size_t i = 0; i < rooms.size(); ++i) {
		const auto& r = rooms[i];
		ImU32 fill = 0;
		if (layer == 0) {
			fill = r.type == 1 ? IM_COL32(120, 200, 255, 60) : r.type == 2 ? IM_COL32(40, 90, 230, 90) : IM_COL32(255, 210, 120, 60);
		} else if (layer >= 1 && layer <= 4 && counts[i] > 0 && largest > 0) {
			static const ImU32 colours[] = {IM_COL32(255, 160, 0, 255), IM_COL32(80, 220, 60, 255), IM_COL32(190, 110, 255, 255), IM_COL32(230, 40, 150, 255)};
			const float strength = 0.15f + 0.55f * static_cast<float>(counts[i]) / static_cast<float>(largest);
			fill = (colours[layer - 1] & 0x00FFFFFF) | (static_cast<ImU32>(strength * 255) << 24);
		}
		if (fill) {
			d->AddRectFilled(at(static_cast<float>(r.left), static_cast<float>(r.top)), at(static_cast<float>(r.right), static_cast<float>(r.bottom)), fill);
		}
		d->AddRect(at(static_cast<float>(r.left), static_cast<float>(r.top)), at(static_cast<float>(r.right), static_cast<float>(r.bottom)), IM_COL32(255, 255, 255, 50));
	}
	// the view's own rectangle
	{
		const auto* v = &*g_engine_context.viewport;
		const float x0 = v->window_x_to_world_x(0), y0 = v->window_y_to_world_y(0);
		const float x1 = v->window_x_to_world_x(ImGui::GetMainViewport()->Size.x), y1 = v->window_y_to_world_y(ImGui::GetMainViewport()->Size.y);
		d->AddRect(at(x0, y0), at(x1, y1), IM_COL32(255, 255, 255, 160));
	}
	for (const auto& egg : eggs) {
		const ImVec2 p = at(egg.x, egg.y - 4);
		d->AddEllipseFilled(p, ImVec2(3, 4), IM_COL32(255, 245, 200, 255));
		d->AddEllipse(p, ImVec2(3, 4), IM_COL32(90, 70, 40, 255));
	}
	const Creature* chosen = subject();
	Creature* pointed = nullptr;
	const ImVec2 mouse = ImGui::GetMousePos();
	for (const auto& dot : dots) {
		const ImVec2 p = at(dot.at.x, dot.at.y - 20); // a little above the feet
		const float r = dot.creature == chosen ? 5.f : 3.5f;
		d->AddCircleFilled(p, r, dot.colour);
		d->AddCircle(p, r, dot.creature == chosen ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 200), 0, dot.creature == chosen ? 2.f : 1.f);
		if (hovering && std::abs(mouse.x - p.x) <= 6 && std::abs(mouse.y - p.y) <= 6) {
			pointed = dot.creature;
		}
	}
	if (pointed) {
		ImGui::SetTooltip("%s%s", creature_name(*pointed).c_str(), pointed->dead ? " (dead)" : "");
		if (clicked) {
			g_engine_context.world->current_norn = pointed->uid;
			g_engine_context.viewport->centre_on(pointed->ground_x(), pointed->ground_y() - 40);
		}
	} else if (clicked) {
		// anywhere else: look there
		g_engine_context.viewport->centre_on((mouse.x - origin.x) / scale, (mouse.y - origin.y) / scale);
	}
	ImGui::EndChild();
	// what the colours mean, as a key: the layer's colours, then the dots
	struct Key {
		ImU32 colour;
		const char* label;
		bool dot;
	};
	std::vector<Key> keys;
	switch (layer) {
		case 0: keys = {{IM_COL32(120, 200, 255, 160), "Surface", false}, {IM_COL32(40, 90, 230, 200), "Under the sea", false}, {IM_COL32(255, 210, 120, 160), "Indoors", false}}; break;
		case 1: keys = {{IM_COL32(255, 160, 0, 255), "Creatures", false}}; break;
		case 2: keys = {{IM_COL32(80, 220, 60, 255), "Food and drink", false}}; break;
		case 3: keys = {{IM_COL32(190, 110, 255, 255), "Toys", false}}; break;
		case 4: keys = {{IM_COL32(230, 40, 150, 255), "Infected", false}}; break;
		default: break;
	}
	keys.push_back({IM_COL32(60, 230, 60, 255), "Norns", true});
	keys.push_back({IM_COL32(200, 90, 40, 255), "Grendels", true});
	keys.push_back({IM_COL32(240, 220, 60, 255), "Others", true});
	keys.push_back({lc_theme().bar_frame, "Dead", true});
	const float line = ImGui::GetTextLineHeight();
	for (size_t i = 0; i < keys.size(); ++i) {
		const float w = line + 4 + ImGui::CalcTextSize(keys[i].label).x;
		if (i > 0) {
			if (ImGui::GetItemRectMax().x + 16 + w < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) {
				ImGui::SameLine(0, 16);
			}
		}
		const ImVec2 p = ImGui::GetCursorScreenPos();
		auto* d = ImGui::GetWindowDrawList();
		if (keys[i].dot) {
			d->AddCircleFilled(ImVec2(p.x + line / 2, p.y + line / 2), line * 0.28f, keys[i].colour);
			d->AddCircle(ImVec2(p.x + line / 2, p.y + line / 2), line * 0.28f, IM_COL32(0, 0, 0, 200));
		} else {
			d->AddRectFilled(ImVec2(p.x + line * 0.1f, p.y + line * 0.15f), ImVec2(p.x + line * 0.9f, p.y + line * 0.85f), keys[i].colour, 3);
		}
		ImGui::Dummy(ImVec2(line, line));
		ImGui::SameLine(0, 4);
		ImGui::TextDisabled("%s", keys[i].label);
	}
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
	if (lc_look() && begin_page("Map")) {
		lc_world_map();
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
	// (autosaves, three beside each save, are in Load world)
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

// LibreCreatures' Injector Kit: Cobs, the add-ons (a find box over the list, each one's
// picture, what's left of it and any warnings, Inject and Remove), and Analysis, what the
// chosen one does, read from its scripts.
struct LcCob {
	fs::path file;
	std::optional<c1cobfile> cob;
	std::string error;
};
std::vector<LcCob> s_lc_cobs;
uint64_t s_lc_cobs_generation = ~0ull;
int s_lc_cob = -1;
char s_lc_find[64] = "";

void lc_load_cobs(bool again = false) {
	if (!again && s_lc_cobs_generation == g_engine_context.generation()) {
		return;
	}
	s_lc_cobs_generation = g_engine_context.generation();
	s_lc_cobs.clear();
	for (const auto& file : list_injectable_cobs()) {
		if (to_ascii_lowercase(file.extension().string()) != ".cob") {
			continue; // removers are the Remove button
		}
		LcCob entry{file, std::nullopt, ""};
		try {
			entry.cob = read_c1cobfile(g_engine_context.paths->find_path(PATH_TYPE_MAIN, file.string()));
		} catch (const std::exception& e) {
			entry.error = e.what();
		}
		s_lc_cobs.push_back(std::move(entry));
	}
	if (s_lc_cob >= static_cast<int>(s_lc_cobs.size()) || (s_lc_cob < 0 && !s_lc_cobs.empty())) {
		s_lc_cob = s_lc_cobs.empty() ? -1 : 0;
	}
}

std::string lc_cob_name(const LcCob& entry) {
	return entry.cob && !entry.cob->name.empty() ? entry.cob->name : entry.file.stem().string();
}

std::string lc_cob_left(const LcCob& entry) {
	try {
		const int left = cob_remaining(entry.file);
		return left < 0 ? "Any" : std::to_string(left);
	} catch (const std::exception&) {
		return "?";
	}
}

// the list, with its find box, filling the given height; returns the chosen add-on or null
const LcCob* lc_cob_list(float width, float height) {
	lc_load_cobs();
	ImGui::BeginGroup();
	ImGui::SetNextItemWidth(width);
	ImGui::InputTextWithHint("##find", "Find", s_lc_find, sizeof(s_lc_find));
	const std::string find = to_ascii_lowercase(std::string(s_lc_find));
	if (ImGui::BeginTable("##cobs", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY,
			ImVec2(width, height - ImGui::GetFrameHeightWithSpacing()))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("COB", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Left ").x);
		ImGui::TableHeadersRow();
		for (int i = 0; i < static_cast<int>(s_lc_cobs.size()); ++i) {
			const auto& entry = s_lc_cobs[static_cast<size_t>(i)];
			const std::string name = lc_cob_name(entry);
			if (!find.empty() && to_ascii_lowercase(name).find(find) == std::string::npos) {
				continue;
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (ImGui::Selectable(fmt::format("{}##cob{}", name, i).c_str(), s_lc_cob == i, ImGuiSelectableFlags_SpanAllColumns)) {
				s_lc_cob = i;
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(lc_cob_left(entry).c_str());
		}
		ImGui::EndTable();
	}
	ImGui::EndGroup();
	return s_lc_cob >= 0 ? &s_lc_cobs[static_cast<size_t>(s_lc_cob)] : nullptr;
}

std::vector<std::string> lc_cob_warnings(const c1cobfile& cob) {
	namespace lc = openc1::librecreatures;
	const auto w = lc::cob_warnings(cob.object_scripts, cob.install_scripts);
	std::vector<std::string> out;
	if (w.kills_creature) {
		out.push_back("Kills the chosen creature");
	}
	if (w.fires_neurons) {
		out.push_back("Fires neurons in a creature's brain directly");
	}
	if (cob.expiration_year) {
		const std::time_t now = std::time(nullptr);
		const std::tm* today = std::localtime(&now);
		const int date = (today->tm_year + 1900) * 10000 + (today->tm_mon + 1) * 100 + today->tm_mday;
		if (date > static_cast<int>(cob.expiration_year * 10000 + cob.expiration_month * 100 + cob.expiration_day)) {
			out.push_back(fmt::format("Past its use-by date ({}/{}/{})", cob.expiration_day, cob.expiration_month, cob.expiration_year));
		}
	}
	if (w.makes_nothing) {
		out.push_back("Makes no object: it only adds scripts");
	}
	return out;
}

void lc_cobs_page() {
	const float h = ImGui::GetContentRegionAvail().y - lc_footer();
	const float list_w = std::max(220.f, ImGui::GetContentRegionAvail().x * 0.42f);
	const LcCob* entry = lc_cob_list(list_w, h);
	ImGui::SameLine();
	ImGui::BeginChild("##cob", ImVec2(0, h));
	if (s_lc_cobs.empty()) {
		lc_note("No COBs", "There are no .cob files in the game's folder. Put add-ons there and they're listed here.");
	} else if (!entry) {
		lc_note("Pick a COB", "Choose an add-on from the list to see it, and inject it into the world.");
	} else if (!entry->cob) {
		ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1), "Couldn't read %s", entry->file.filename().string().c_str());
		ImGui::TextWrapped("%s", entry->error.c_str());
	} else {
		const auto& cob = *entry->cob;
		if (cob.picture) {
			std::vector<uint8_t> pixels(cob.picture.data.data(), cob.picture.data.data() + static_cast<size_t>(cob.picture.width) * static_cast<size_t>(cob.picture.height));
			const auto* picture = UIImages::indexed_image("cob:" + entry->file.string(), cob.picture.width, cob.picture.height, pixels);
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(static_cast<float>(cob.picture.width) * 2, static_cast<float>(cob.picture.height) * 2));
			UIImages::draw_bitmap(ImGui::GetWindowDrawList(), picture, pos, 2);
		}
		ImGui::TextColored(lc_theme().heading, "%s", lc_cob_name(*entry).c_str());
		const std::string left = lc_cob_left(*entry);
		ImGui::TextDisabled("Quantity remaining: %s", left == "Any" ? "unlimited" : left.c_str());
		if (openc1::librecreatures::needs_creature(cob.install_scripts)) {
			ImGui::TextDisabled("It acts on the chosen creature.");
		}
		const auto warnings = lc_cob_warnings(cob);
		if (!warnings.empty()) {
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1), "Warnings");
			for (const auto& w : warnings) {
				ImGui::BulletText("%s", w.c_str());
			}
		}
		ImGui::Spacing();
		ImGui::BeginDisabled(left == "0");
		if (ImGui::Button("Inject")) {
			try {
				inject_cob(entry->file);
				s_kit_error.clear();
			} catch (const std::exception& e) {
				s_kit_error = e.what();
			}
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Remove")) {
			try {
				remove_cob(entry->file);
				s_kit_error.clear();
			} catch (const std::exception& e) {
				s_kit_error = e.what();
			}
		}
		ImGui::SetItemTooltip("Takes it out of the world: by its .rcb file if it has one, otherwise by removing what it made and the scripts it added");
	}
	ImGui::EndChild();
}

const char* lc_event_name(int event) {
	switch (event) {
		case 0: return "Deactivate";
		case 1: return "Activate 1";
		case 2: return "Activate 2";
		case 3: return "Hit";
		case 4: return "Picked up";
		case 5: return "Dropped";
		case 6: return "Collision";
		case 7: return "Created";
		case 8: return "Removed";
		case 9: return "Timer";
		case 16: return "A creature is quiescent with it";
		case 17: return "A creature activates it (1)";
		case 18: return "A creature activates it (2)";
		case 19: return "A creature deactivates it";
		case 20: return "A creature approaches it";
		case 21: return "A creature retreats from it";
		case 22: return "A creature picks it up";
		case 23: return "A creature drops it";
		case 24: return "A creature needs it";
		case 25: return "A creature rests on it";
		case 26: return "A creature walks west";
		case 27: return "A creature walks east";
		default: return nullptr;
	}
}

void lc_analysis_page() {
	namespace lc = openc1::librecreatures;
	const float h = ImGui::GetContentRegionAvail().y - lc_footer();
	const float list_w = std::max(220.f, ImGui::GetContentRegionAvail().x * 0.42f);
	const LcCob* entry = lc_cob_list(list_w, h);
	ImGui::SameLine();
	ImGui::BeginChild("##analysis", ImVec2(0, h), ImGuiChildFlags_Borders);
	if (!entry || !entry->cob) {
		ImGui::TextDisabled("Pick a COB to see what it does.");
		ImGui::EndChild();
		return;
	}
	const auto& cob = *entry->cob;
	const auto commands = [](const std::string& script) {
		size_t from = 0;
		while (from < script.size()) {
			size_t comma = script.find(',', from);
			if (comma == std::string::npos) {
				comma = script.size();
			}
			if (comma > from) {
				ImGui::BulletText("%s", script.substr(from, comma - from).c_str());
			}
			from = comma + 1;
		}
	};
	const auto classifier = [](const lc::Classifier& c) { return fmt::format("{} {} {}", c.family, c.genus, c.species); };
	ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
	if (ImGui::TreeNode("General")) {
		ImGui::Text("File \"%s\"", entry->file.filename().string().c_str());
		for (const auto& made : lc::created_classifiers(cob.install_scripts)) {
			ImGui::Text("Makes object %s", classifier(made).c_str());
		}
		ImGui::Text("Quantity remaining: %s", lc_cob_left(*entry).c_str());
		if (cob.expiration_year) {
			ImGui::Text("Use by: %u/%u/%u", cob.expiration_day, cob.expiration_month, cob.expiration_year);
		}
		ImGui::TreePop();
	}
	ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
	if (ImGui::TreeNode("Scripts")) {
		for (size_t i = 0; i < cob.object_scripts.size(); ++i) {
			lc::InstalledScript header{};
			std::string label = "Object script";
			if (lc::script_header(cob.object_scripts[i], header)) {
				const char* event = lc_event_name(header.event);
				label = fmt::format("{}: object {}", event ? event : fmt::format("Event {}", header.event).c_str(), classifier(header.classifier));
			}
			if (ImGui::TreeNode(fmt::format("{}##object{}", label, i).c_str())) {
				commands(cob.object_scripts[i]);
				ImGui::TreePop();
			}
		}
		for (size_t i = 0; i < cob.install_scripts.size(); ++i) {
			if (ImGui::TreeNode(fmt::format("Injection script {}##install{}", i + 1, i).c_str())) {
				commands(cob.install_scripts[i]);
				ImGui::TreePop();
			}
		}
		ImGui::TreePop();
	}
	ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
	if (ImGui::TreeNode("Chemicals affected")) {
		const auto chemicals = lc::affected_chemicals(cob.object_scripts, cob.install_scripts);
		if (chemicals.empty()) {
			ImGui::TextDisabled("None");
		}
		for (const int chemical : chemicals) {
			ImGui::Text("%3d  %s", chemical, chemical_names()[static_cast<size_t>(chemical)].c_str());
		}
		ImGui::TreePop();
	}
	ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
	if (ImGui::TreeNode("Warnings")) {
		const auto warnings = lc_cob_warnings(cob);
		if (warnings.empty()) {
			ImGui::TextDisabled("None");
		}
		for (const auto& w : warnings) {
			ImGui::TextUnformatted(w.c_str());
		}
		ImGui::TreePop();
	}
	ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
	if (ImGui::TreeNode("Removal")) {
		auto remover = entry->file;
		remover.replace_extension(".rcb");
		if (!g_engine_context.paths->find_path(PATH_TYPE_MAIN, remover.string()).empty()) {
			ImGui::Text("By its removal file, %s", remover.filename().string().c_str());
		} else {
			const std::string generated = lc::generated_removal(cob.object_scripts, cob.install_scripts);
			if (generated.empty()) {
				ImGui::TextDisabled("Nothing to remove");
			} else if (ImGui::TreeNode("No .rcb file: Remove runs this")) {
				commands(generated);
				ImGui::TreePop();
			}
		}
		ImGui::TreePop();
	}
	ImGui::EndChild();
}

void draw_injector_kit() {
	if (!ImGui::BeginTabBar("injector")) {
		return;
	}
	if (lc_look()) {
		if (begin_page("Cobs")) {
			lc_cobs_page();
			ImGui::EndTabItem();
		}
		if (begin_page("Analysis")) {
			lc_analysis_page();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
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
		case KIT_HATCHERY: draw_hatchery_kit(); break;
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
	lc_run_repeat(); // a repeating injection keeps going with the kit closed
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
		const bool fits_picture = k == KIT_HATCHERY && !lc_look(); // LibreCreatures' has a Designer page too
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
	static const PaneInfo lab{"Lab", {{"Chemistry", KIT_SCIENCE}, {"Brain", KIT_SCIENCE}, {"Decisions", KIT_SCIENCE}, {"Genes", KIT_SCIENCE}, {"Injections", KIT_SCIENCE}}};
	static const PaneInfo objects{"Objects", {{"Hatchery", KIT_HATCHERY}, {"Designer", KIT_HATCHERY}, {"Add-ons", KIT_INJECTOR}, {"Analysis", KIT_INJECTOR}, {"Herbs", KIT_HEALTH}}};
	static const PaneInfo records{"Records", {{"Population", KIT_OBSERVATION}, {"Map", KIT_OBSERVATION}, {"Deaths", KIT_FUNERAL}, {"Graveyard", KIT_FUNERAL}, {"World", KIT_SCORE}}};
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
		case KIT_INJECTOR: return {Pane::Objects, 2};
		case KIT_OBSERVATION: return {Pane::Records, 0};
		case KIT_FUNERAL: return {Pane::Records, 2};
		case KIT_SCORE:
		case KIT_BACKUP: return {Pane::Records, 4};
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
			// each by its sign
			const float h = ImGui::GetTextLineHeight();
			const ImVec2 at = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(h, h));
			UIArt::glyph(ImGui::GetWindowDrawList(), UIArt::drive_glyph(d), ImVec2(at.x + h / 2, at.y + h / 2), h * 0.9f, ImGui::GetColorU32(ImGuiCol_Text));
			ImGui::SameLine();
			wide_meter(Kits::chemical_name(d + 1), c.loci.drives[d] / 255.f, UIStyle::health_colour(1.f - c.loci.drives[d] / 255.f));
		}
		ImGui::SeparatorText("Temperature");
		chemical_bar(c, 4);
		chemical_bar(c, 5);
		ImGui::EndTable();
	}
}

// The modern panes show LibreCreatures' pages with our facelift (s_lc_in_pane): the Health
// Kit's fitness readings beside every drive, the Breeder's hormones and their graph, the
// Science Kit's chemistry graph, brain map, decisions, genome and injections, the Hatchery's
// Designer, the Injector's analysis, the Ecology Kit's map, and the Funeral Kit's deaths
// and graveyard.
void norn_health(Creature& c) {
	const float k = UIStyle::scale();
	if (begin_columns("##health", 2, 300 * k)) {
		lc_fitness(c);
		next_column();
		lc_heading("Drives and needs");
		lc_drives(c);
		ImGui::EndTable();
	}
}

void norn_breeding(Creature& c) {
	const float k = UIStyle::scale();
	if (begin_columns("##breeding", 2, 300 * k)) {
		lc_fertility(c);
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
	lc_biochemistry(c);
}

void lab_brain(Creature& c) {
	lc_brain_map(c);
}

void lab_decisions(Creature& c) {
	const float k = UIStyle::scale();
	if (begin_columns("##decisions", 2, 300 * k)) {
		lc_decisions(c);
		next_column();
		lc_heading("Brain activity");
		lc_brain(c);
		ImGui::EndTable();
	}
}

void lab_genes(Creature& c) {
	lc_genetics(c);
}

void lab_injections(Creature& c) {
	lc_injections(c);
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

void records_deaths() {
	lc_deaths();
	if (s_visit_graveyard) {
		// Visit Headstone: to the Graveyard section, at its stone
		s_visit_graveyard = false;
		open_pane(Pane::Records, 3);
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
	// LibreCreatures' pages, with our facelift
	s_lc_in_pane = true;
	struct Done {
		~Done() { s_lc_in_pane = false; }
	} done;
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
				case 2: lab_decisions(*c); break;
				case 3: lab_genes(*c); break;
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
				case 1: lc_designer_page(); break;
				case 2: objects_injector(); break;
				case 3: lc_analysis_page(); break;
				default: objects_herbs(); break;
			}
			break;
		case Pane::Records:
			switch (section) {
				case 0: records_population(); break;
				case 1: lc_world_map(); break;
				case 2: records_deaths(); break;
				case 3: lc_graveyard(); break;
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

// The panes' colours, for paper: brown ink, fields of lighter paper, wood buttons, and brass
// scroll bars. Returns how many to pop.
int push_paper_colours() {
	const std::pair<ImGuiCol, ImVec4> colours[] = {
		{ImGuiCol_Text, ImVec4(0.21f, 0.15f, 0.09f, 1)},
		{ImGuiCol_TextDisabled, ImVec4(0.47f, 0.39f, 0.30f, 1)},
		{ImGuiCol_FrameBg, ImVec4(0.98f, 0.95f, 0.88f, 1)},
		{ImGuiCol_FrameBgHovered, ImVec4(1.00f, 0.98f, 0.93f, 1)},
		{ImGuiCol_FrameBgActive, ImVec4(0.96f, 0.91f, 0.81f, 1)},
		{ImGuiCol_Button, ImVec4(0.79f, 0.65f, 0.44f, 1)},
		{ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.71f, 0.50f, 1)},
		{ImGuiCol_ButtonActive, ImVec4(0.72f, 0.57f, 0.37f, 1)},
		{ImGuiCol_Header, ImVec4(0.86f, 0.77f, 0.62f, 1)},
		{ImGuiCol_HeaderHovered, ImVec4(0.90f, 0.82f, 0.68f, 1)},
		{ImGuiCol_HeaderActive, ImVec4(0.82f, 0.72f, 0.56f, 1)},
		{ImGuiCol_TableHeaderBg, ImVec4(0.84f, 0.75f, 0.60f, 1)},
		{ImGuiCol_TableBorderStrong, ImVec4(0.62f, 0.49f, 0.33f, 1)},
		{ImGuiCol_TableBorderLight, ImVec4(0.78f, 0.69f, 0.55f, 1)},
		{ImGuiCol_TableRowBg, ImVec4(0, 0, 0, 0)},
		{ImGuiCol_TableRowBgAlt, ImVec4(0.45f, 0.30f, 0.12f, 0.07f)},
		{ImGuiCol_Separator, ImVec4(0.66f, 0.54f, 0.38f, 1)},
		{ImGuiCol_Border, ImVec4(0.62f, 0.49f, 0.33f, 0.6f)},
		{ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0)},
		{ImGuiCol_ScrollbarGrab, ImVec4(0.69f, 0.54f, 0.33f, 1)},
		{ImGuiCol_ScrollbarGrabHovered, ImVec4(0.62f, 0.47f, 0.27f, 1)},
		{ImGuiCol_ScrollbarGrabActive, ImVec4(0.55f, 0.40f, 0.22f, 1)},
		{ImGuiCol_CheckMark, ImVec4(0.27f, 0.45f, 0.16f, 1)},
		{ImGuiCol_SliderGrab, ImVec4(0.58f, 0.42f, 0.18f, 1)},
		{ImGuiCol_SliderGrabActive, ImVec4(0.50f, 0.35f, 0.14f, 1)},
		{ImGuiCol_PlotLines, ImVec4(0.42f, 0.27f, 0.10f, 1)},
		{ImGuiCol_PlotHistogram, ImVec4(0.62f, 0.40f, 0.12f, 1)},
		{ImGuiCol_Tab, ImVec4(0.84f, 0.75f, 0.60f, 1)},
		{ImGuiCol_TabHovered, ImVec4(0.90f, 0.82f, 0.68f, 1)},
		{ImGuiCol_TabSelected, ImVec4(0.95f, 0.91f, 0.81f, 1)},
		{ImGuiCol_PopupBg, ImVec4(0.96f, 0.93f, 0.85f, 1)},
		{ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0)},
	};
	for (const auto& [which, colour] : colours) {
		ImGui::PushStyleColor(which, colour);
	}
	return static_cast<int>(std::size(colours));
}

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
	// as tall as the room between the clock and the bar (a short screen's pane scrolls instead)
	const float height = std::max(160.f, io.DisplaySize.y - s_panel_top - s_panel_bottom - 2 * margin);
	// it slides in from the right as it opens or changes pane, and fades in
	static Pane shown = Pane::None;
	static double opened = 0;
	if (s_pane != shown) {
		shown = s_pane;
		opened = ImGui::GetTime();
	}
	float t = std::clamp(static_cast<float>((ImGui::GetTime() - opened) / 0.2), 0.f, 1.f);
	t = t * t * (3 - 2 * t);
	const ImVec2 pos(io.DisplaySize.x - width - margin + (1 - t) * 36 * k, s_panel_top + margin);
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	const float border = 14 * k;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(border + 8 * k, border + 10 * k));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.05f, t));
	if (ImGui::Begin("##pane", nullptr, flags)) {
		auto* draw = ImGui::GetWindowDrawList();
		const int first = draw->VtxBuffer.Size;
		UIArt::window_frame(draw, pos, ImVec2(pos.x + width, pos.y + height), border, info.name, true);
		for (int i = first; i < draw->VtxBuffer.Size; ++i) {
			ImU32& col = draw->VtxBuffer[i].col;
			const ImU32 a = static_cast<ImU32>(static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) * t);
			col = (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
		}
		// close, at the top right of the frame
		const ImVec2 close(pos.x + width - border * 1.6f, pos.y + border * 0.9f);
		const float cr = border * 0.75f, reach = cr * 1.8f; // a finger's worth round it
		ImGui::SetCursorScreenPos(ImVec2(close.x - reach, close.y - reach));
		if (ImGui::InvisibleButton("##closepane", ImVec2(reach * 2, reach * 2))) {
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

		// The section itself, on the pane's paper in brown ink. Its text stays full size, and
		// the page scrolls when it's longer than the pane.
		const int colours = push_paper_colours();
		ImGui::BeginChild("##section", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
		s_current_kit = info.sections[static_cast<size_t>(s_section)].kit;
		s_page_index = 0;
		draw_section(s_pane, s_section);
		ImGui::EndChild();
		ImGui::PopStyleColor(colours);
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
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
