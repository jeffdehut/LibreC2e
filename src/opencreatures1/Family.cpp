#include "Family.h"

#include "EngineContext.h"
#include "sdlbackend/SDLTouch.h"
#include "UIImages.h"
#include "ImageManager.h"
#include "PathManager.h"
#include "PhotoAlbum.h"
#include "UIStyle.h"
#include "WorldState.h"
#include "WorldSystems.h"
#include "Wolfling.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <imgui.h>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace Family {

namespace {

std::map<std::string, Member> s_members;
std::set<std::string> s_photographed; // "moniker stage", stage -1 for the birth photo
bool s_loaded = false;
bool s_changed = false;
bool s_show_tree = false;
bool s_layout_dirty = true; // the tree's layout needs making again
bool s_fit_next = true; // the tree's view fits the whole family next time
std::string s_focus; // the Norn in the middle of the tree

fs::path folder() {
	return g_engine_context.paths->find_path(PATH_TYPE_MAIN, "Creatures.exe").parent_path();
}

// Each world has its own family, next to its world file (World.openc1.sfc ->
// World.openc1.family.txt); Save As copies it. A world without one yet starts from the file
// all worlds used to share (kept as it was), with just its own Norns and their ancestors.
fs::path family_path() {
	return folder() / (fs::path(get_world_file_name()).stem().string() + ".family.txt");
}

// the shipped Hatchery and starting genomes aren't Norns of their own
bool is_founder(const std::string& m) {
	return m.empty() || m == "EVE_" || m == "ADAM" || m.rfind("mum", 0) == 0 || m.rfind("dad", 0) == 0 || m == std::string(4, '\0');
}

void load() {
	s_loaded = true;
	s_members.clear();
	s_photographed.clear();
	const bool own = fs::exists(family_path());
	std::ifstream in(own ? family_path() : folder() / "Family.openc1.txt");
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream s(line);
		std::string key;
		std::getline(s, key, '|');
		if (key == "norn") {
			Member m;
			std::string sex, dead;
			std::getline(s, m.moniker, '|');
			std::getline(s, m.mother, '|');
			std::getline(s, m.father, '|');
			std::getline(s, sex, '|');
			std::getline(s, dead, '|');
			std::getline(s, m.name);
			m.sex = std::atoi(sex.c_str());
			m.dead = dead == "1";
			s_members[m.moniker] = m;
		} else if (key == "face") {
			std::string moniker, sprite, base, count, frame;
			std::getline(s, moniker, '|');
			std::getline(s, sprite, '|');
			std::getline(s, base, '|');
			std::getline(s, count, '|');
			std::getline(s, frame);
			Member& m = s_members[moniker];
			m.face_sprite = sprite;
			m.face_base = std::atoi(base.c_str());
			m.face_count = std::atoi(count.c_str());
			m.face_frame = std::atoi(frame.c_str());
		} else if (key == "photo") {
			std::string rest;
			std::getline(s, rest);
			s_photographed.insert(rest);
		}
	}
	if (!own) {
		// from the shared file: keep this world's Norns and their ancestors
		std::set<std::string> keep;
		std::vector<std::string> todo;
		for (auto* obj : *g_engine_context.objects) {
			if (auto* c = obj->as_creature(); c && c->genus == 1) {
				todo.push_back(c->moniker);
			}
		}
		while (!todo.empty()) {
			const std::string k = todo.back();
			todo.pop_back();
			auto it = s_members.find(k);
			if (k.empty() || !keep.insert(k).second || it == s_members.end()) {
				continue;
			}
			todo.push_back(it->second.mother);
			todo.push_back(it->second.father);
		}
		for (auto it = s_members.begin(); it != s_members.end();) {
			it = keep.count(it->first) ? std::next(it) : s_members.erase(it);
		}
		for (auto it = s_photographed.begin(); it != s_photographed.end();) {
			it = keep.count(it->substr(0, it->find(' '))) ? std::next(it) : s_photographed.erase(it);
		}
		s_changed = true;
	}
	s_layout_dirty = true;
	s_fit_next = true;
}

void save() {
	std::ofstream out(family_path());
	for (auto& [k, m] : s_members) {
		out << "norn|" << m.moniker << "|" << m.mother << "|" << m.father << "|" << m.sex << "|" << (m.dead ? 1 : 0) << "|" << m.name << "\n";
	}
	for (auto& [k, m] : s_members) {
		if (!m.face_sprite.empty()) {
			out << "face|" << m.moniker << "|" << m.face_sprite << "|" << m.face_base << "|" << m.face_count << "|" << m.face_frame << "\n";
		}
	}
	for (auto& p : s_photographed) {
		out << "photo|" << p << "\n";
	}
	s_changed = false;
}

std::string label(const Member& m) {
	return m.name.empty() || m.name == "<Un Named>" ? m.moniker : m.name;
}

void update_registry() {
	for (auto& [k, m] : s_members) {
		m.here = false;
	}
	for (auto* obj : *g_engine_context.objects) {
		auto* c = obj->as_creature();
		if (!c || c->genus != 1) {
			continue;
		}
		Member& m = s_members[c->moniker];
		const Member before = m;
		m.moniker = c->moniker;
		m.mother = c->mother;
		m.father = c->father;
		m.sex = c->sex;
		m.name = c->history_name;
		m.dead = c->dead != 0;
		m.here = true;
		// remember its face (it changes as it grows) for when it's gone
		if (c->head && !m.dead) {
			const auto& g = c->head->gallery();
			const int32_t frame = c->head->base() + 8;
			if (!g.name.empty() && (g.name != m.face_sprite || frame != m.face_frame || g.absolute_base != m.face_base || g.size() != m.face_count)) {
				m.face_sprite = g.name;
				m.face_base = g.absolute_base;
				m.face_count = g.size();
				m.face_frame = frame;
				s_changed = true;
			}
		}
		if (m.mother != before.mother || m.father != before.father || m.name != before.name || m.dead != before.dead || m.sex != before.sex) {
			s_changed = true;
		}
	}
}

// a photo when a Norn is born and when it reaches each new life stage (baby, child,
// adolescent, youth, adult, old, senile)
void automatic_photos() {
	for (auto* obj : *g_engine_context.objects) {
		auto* c = obj->as_creature();
		if (!c || c->genus != 1 || c->dead || c->movement_status != MOVEMENT_AUTONOMOUS) {
			continue;
		}
		std::string key;
		if (c->age == 0) {
			// only newborns get a birth photo, not babies already in a loaded world
			if (c->age_ticks > 1200) {
				continue;
			}
			key = c->moniker + " -1";
		} else {
			key = fmt::format("{} {}", c->moniker, c->age);
		}
		if (s_photographed.count(key)) {
			continue;
		}
		if (PhotoAlbum::take_photo_quietly(*c)) {
			s_photographed.insert(key);
			s_changed = true;
			fmt::print("INFO [Family] automatic photo of {} ({})\n", c->moniker, c->age == 0 ? "born" : fmt::format("life stage {}", c->age));
		}
		return; // one at a time
	}
}

// --- the tree ---
//
// Every Norn the game folder has seen, in one view that can be dragged and zoomed: a row per
// generation (the Hatchery's Norns first), each Norn a round face with its name, lines from
// parents to children, and the chosen Norn's ancestors and descendants picked out.

ImU32 with_alpha(ImU32 colour, float a) {
	const ImU32 alpha = static_cast<ImU32>(static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF) * a);
	return (colour & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

std::vector<std::string> children_of(const std::string& moniker) {
	std::vector<std::string> kids;
	for (auto& [k, m] : s_members) {
		if (m.mother == moniker || m.father == moniker) {
			kids.push_back(k);
		}
	}
	return kids;
}

struct Layout {
	std::map<std::string, ImVec2> at; // tree coordinates
	std::map<std::string, int> depth;
	int rows = 0;
	ImVec2 min{0, 0}, max{0, 0};
};
Layout s_layout;
size_t s_layout_members = SIZE_MAX; // the registry size it was made for
float s_zoom = 1;
ImVec2 s_pan{0, 0}; // the tree point at the view's top-left
constexpr float NODE_GAP = 96, ROW_GAP = 120, FACE_R = 26;

bool is_member(const std::string& moniker) {
	return !moniker.empty() && s_members.count(moniker);
}

int depth_of(const std::string& moniker, std::map<std::string, int>& memo, int guard = 0) {
	if (auto it = memo.find(moniker); it != memo.end()) {
		return it->second;
	}
	const Member& m = s_members[moniker];
	int d = 0;
	if (guard < 64) {
		if (is_member(m.mother)) d = std::max(d, depth_of(m.mother, memo, guard + 1) + 1);
		if (is_member(m.father)) d = std::max(d, depth_of(m.father, memo, guard + 1) + 1);
	}
	memo[moniker] = d;
	return d;
}

void make_layout() {
	s_layout = Layout{};
	for (auto& [k, m] : s_members) {
		depth_of(k, s_layout.depth);
	}
	std::map<int, std::vector<std::string>> rows;
	for (auto& [k, d] : s_layout.depth) {
		rows[d].push_back(k);
	}
	s_layout.rows = rows.empty() ? 0 : rows.rbegin()->first + 1;
	// place a row in the order of its wanted positions, at least NODE_GAP apart, then shift it
	// so that on average each Norn is where it wanted to be
	auto place = [&](std::vector<std::string>& row, int d, const std::map<std::string, float>& want) {
		std::stable_sort(row.begin(), row.end(), [&](const std::string& a, const std::string& b) { return want.at(a) < want.at(b); });
		std::vector<float> x(row.size());
		float shift = 0;
		for (size_t i = 0; i < row.size(); ++i) {
			x[i] = i == 0 ? want.at(row[i]) : std::max(want.at(row[i]), x[i - 1] + NODE_GAP);
			shift += want.at(row[i]) - x[i];
		}
		shift /= static_cast<float>(std::max<size_t>(1, row.size()));
		for (size_t i = 0; i < row.size(); ++i) {
			s_layout.at[row[i]] = ImVec2(x[i] + shift, static_cast<float>(d) * ROW_GAP);
		}
	};
	// first pass, top down: each Norn under its parents
	for (auto& [d, row] : rows) {
		std::map<std::string, float> want;
		float next = 0;
		for (auto& k : row) {
			const Member& m = s_members[k];
			float sum = 0;
			int n = 0;
			for (const std::string* p : {&m.mother, &m.father}) {
				if (auto it = s_layout.at.find(*p); it != s_layout.at.end()) {
					sum += it->second.x;
					n++;
				}
			}
			want[k] = n ? sum / static_cast<float>(n) : (next += NODE_GAP);
		}
		place(row, d, want);
	}
	// second pass, bottom up: parents over their children, then the children again
	for (int sweep = 0; sweep < 2; ++sweep) {
		for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
			std::map<std::string, float> want;
			for (auto& k : it->second) {
				float sum = 0;
				int n = 0;
				for (auto& [c, m] : s_members) {
					if ((m.mother == k || m.father == k) && s_layout.at.count(c)) {
						sum += s_layout.at[c].x;
						n++;
					}
				}
				want[k] = n ? sum / static_cast<float>(n) : s_layout.at[k].x;
			}
			place(it->second, it->first, want);
		}
		for (auto& [d, row] : rows) {
			if (d == 0) {
				continue;
			}
			std::map<std::string, float> want;
			for (auto& k : row) {
				const Member& m = s_members[k];
				float sum = 0;
				int n = 0;
				for (const std::string* p : {&m.mother, &m.father}) {
					if (auto f = s_layout.at.find(*p); f != s_layout.at.end()) {
						sum += f->second.x;
						n++;
					}
				}
				want[k] = n ? sum / static_cast<float>(n) : s_layout.at[k].x;
			}
			place(row, d, want);
		}
	}
	bool first = true;
	for (auto& [k, p] : s_layout.at) {
		if (first) {
			s_layout.min = s_layout.max = p;
			first = false;
		}
		s_layout.min = ImVec2(std::min(s_layout.min.x, p.x), std::min(s_layout.min.y, p.y));
		s_layout.max = ImVec2(std::max(s_layout.max.x, p.x), std::max(s_layout.max.y, p.y));
	}
	s_layout_members = s_members.size();
	s_layout_dirty = false;
}

// the chosen Norn's ancestors and descendants
std::set<std::string> line_of(const std::string& focus) {
	std::set<std::string> line;
	std::vector<std::string> up{focus}, down{focus};
	while (!up.empty()) {
		auto k = up.back();
		up.pop_back();
		if (!line.insert(k).second || !is_member(k)) continue;
		up.push_back(s_members[k].mother);
		up.push_back(s_members[k].father);
	}
	while (!down.empty()) {
		auto k = down.back();
		down.pop_back();
		for (auto& [c, m] : s_members) {
			if ((m.mother == k || m.father == k) && !line.count(c)) {
				line.insert(c);
				down.push_back(c);
			}
		}
	}
	return line;
}

const Creature* creature_here(const std::string& moniker) {
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature(); c && c->moniker == moniker) {
			return c;
		}
	}
	return nullptr;
}

// a remembered face, or nothing
const ImageGallery* remembered_face(const Member& m) {
	static std::map<std::string, const ImageGallery*> cache; // nullptr: couldn't load
	if (m.face_sprite.empty() || m.face_frame < 0) {
		return nullptr;
	}
	const std::string key = fmt::format("{} {} {}", m.face_sprite, m.face_base, m.face_count);
	if (auto it = cache.find(key); it != cache.end()) {
		return it->second;
	}
	const ImageGallery* g = nullptr;
	try {
		if (g_engine_context.images->has_spr_image(m.face_sprite)) {
			g = &g_engine_context.images->get_image(m.face_sprite, m.face_base, m.face_count, ImageManager::IMAGE_SPR);
		}
	} catch (...) {
		g = nullptr;
	}
	cache[key] = g;
	return g;
}

void draw_node(ImDrawList* draw, const Member& m, ImVec2 centre, float r, bool focus, bool in_line, bool dim) {
	const ImU32 ring = m.sex == 2 ? IM_COL32(236, 120, 170, 255) : m.sex == 1 ? IM_COL32(100, 160, 240, 255) : IM_COL32(160, 160, 170, 255);
	const int alpha = dim ? 110 : 255;
	if (focus) {
		draw->AddCircleFilled(centre, r + 7, with_alpha(UIStyle::accent(), 0.35f), 40);
	}
	draw->AddCircleFilled(centre, r, IM_COL32(40, 40, 48, alpha), 40);
	const ImU32 tint = m.dead ? IM_COL32(150, 150, 150, 150) : IM_COL32(255, 255, 255, alpha);
	const float inner = r * 1.72f;
	const ImVec2 a(centre.x - inner / 2, centre.y - inner / 2), b(centre.x + inner / 2, centre.y + inner / 2);
	bool drew = false;
	if (const Creature* c = m.here ? creature_here(m.moniker) : nullptr) {
		drew = UIImages::draw_creature_face(draw, *c, a, b, tint);
	}
	if (!drew) {
		if (const ImageGallery* g = remembered_face(m)) {
			drew = UIImages::draw_face(draw, *g, m.face_frame, a, b, tint);
		}
	}
	if (!drew) {
		// no picture: the first letter of its name
		const std::string initial = label(m).substr(0, 1);
		ImFont* font = ImGui::GetFont();
		const float size = r * 1.1f;
		const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, initial.c_str());
		draw->AddText(font, size, ImVec2(centre.x - ts.x / 2, centre.y - ts.y / 2), IM_COL32(230, 230, 235, alpha), initial.c_str());
	}
	draw->AddCircle(centre, r, focus ? UIStyle::accent() : with_alpha(ring, in_line ? 1.f : dim ? 0.4f : 0.75f), 40, focus ? 3.f : in_line ? 2.5f : 1.5f);
	if (m.dead) {
		// a small cross for a Norn that has died
		const ImVec2 cp(centre.x + r * 0.72f, centre.y - r * 0.72f);
		const float s = r * 0.2f;
		draw->AddCircleFilled(cp, s * 1.5f, IM_COL32(30, 30, 36, 230));
		draw->AddLine(ImVec2(cp.x, cp.y - s), ImVec2(cp.x, cp.y + s), IM_COL32(220, 220, 225, 255), 1.5f);
		draw->AddLine(ImVec2(cp.x - s * 0.7f, cp.y - s * 0.3f), ImVec2(cp.x + s * 0.7f, cp.y - s * 0.3f), IM_COL32(220, 220, 225, 255), 1.5f);
	}
}

void draw_tree() {
	if (s_layout_dirty || s_layout_members != s_members.size()) {
		make_layout();
	}
	if (s_members.empty()) {
		ImGui::TextDisabled("No Norns yet. Hatch one, and its family grows here.");
		return;
	}
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 size = ImGui::GetContentRegionAvail();
	if (size.x < 50 || size.y < 50) {
		return;
	}
	const float margin = FACE_R * 2.5f;
	auto fit = [&] {
		const float w = s_layout.max.x - s_layout.min.x + 2 * margin, h = s_layout.max.y - s_layout.min.y + 2 * margin;
		s_zoom = std::clamp(std::min(size.x / w, size.y / h), 0.15f, 1.6f);
		s_pan = ImVec2(s_layout.min.x - margin - (size.x / s_zoom - w) / 2, s_layout.min.y - margin - (size.y / s_zoom - h) / 2);
	};
	if (s_fit_next) {
		fit();
		s_fit_next = false;
	}
	auto to_screen = [&](ImVec2 p) { return ImVec2(origin.x + (p.x - s_pan.x) * s_zoom, origin.y + (p.y - s_pan.y) * s_zoom); };
	auto zoom_about = [&](float factor, ImVec2 screen) {
		const float z = std::clamp(s_zoom * factor, 0.15f, 3.f);
		const ImVec2 p(s_pan.x + (screen.x - origin.x) / s_zoom, s_pan.y + (screen.y - origin.y) / s_zoom);
		s_zoom = z;
		s_pan = ImVec2(p.x - (screen.x - origin.x) / s_zoom, p.y - (screen.y - origin.y) / s_zoom);
	};

	// the whole area takes drags (pan) and the wheel or a pinch (zoom)
	ImGui::InvisibleButton("##treecanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	auto& io = ImGui::GetIO();
	if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2))) {
		s_pan.x -= io.MouseDelta.x / s_zoom;
		s_pan.y -= io.MouseDelta.y / s_zoom;
	}
	const float pinch = sdl_take_interface_zoom();
	if (hovered) {
		if (io.MouseWheel != 0) {
			zoom_about(std::pow(1.12f, io.MouseWheel), io.MousePos);
		}
		if (pinch != 1) {
			zoom_about(pinch, io.MousePos);
		}
	}

	auto* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
	// an opaque backdrop, so the world doesn't show through the tree
	ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
	bg.w = 1;
	draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), ImGui::GetColorU32(bg), UIStyle::classic() ? 0 : 10);
	const std::set<std::string> line = is_member(s_focus) ? line_of(s_focus) : std::set<std::string>{};
	const float r = FACE_R * s_zoom;

	// generation labels down the left
	for (int d = 0; d < s_layout.rows; ++d) {
		const ImVec2 p = to_screen(ImVec2(0, static_cast<float>(d) * ROW_GAP));
		draw->AddText(ImVec2(origin.x + 8, p.y - ImGui::GetTextLineHeight() / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), fmt::format("G{}", d + 1).c_str());
	}
	// parents to children
	for (auto& [k, m] : s_members) {
		auto child = s_layout.at.find(k);
		if (child == s_layout.at.end()) continue;
		for (const std::string* p : {&m.mother, &m.father}) {
			auto parent = s_layout.at.find(*p);
			if (parent == s_layout.at.end()) continue;
			const bool on_line = line.count(k) && line.count(*p);
			const ImVec2 a = to_screen(parent->second), b = to_screen(child->second);
			const ImVec2 a2(a.x, a.y + r), b2(b.x, b.y - r);
			const float bend = (b2.y - a2.y) * 0.5f;
			draw->AddBezierCubic(a2, ImVec2(a2.x, a2.y + bend), ImVec2(b2.x, b2.y - bend), b2,
				on_line ? UIStyle::accent() : ImGui::GetColorU32(ImGuiCol_TextDisabled, line.empty() ? 0.7f : 0.35f), on_line ? 2.5f : 1.2f);
		}
	}
	// the Norns
	std::string hovered_norn;
	for (auto& [k, p] : s_layout.at) {
		const ImVec2 c = to_screen(p);
		if (c.x < origin.x - r * 3 || c.x > origin.x + size.x + r * 3 || c.y < origin.y - r * 3 || c.y > origin.y + size.y + r * 3) {
			continue;
		}
		const Member& m = s_members[k];
		const bool focus = k == s_focus;
		draw_node(draw, m, c, r, focus, line.count(k) > 0, !line.empty() && !line.count(k));
		if (s_zoom > 0.45f) {
			const std::string name = label(m);
			const ImVec2 ts = ImGui::CalcTextSize(name.c_str());
			draw->AddText(ImVec2(c.x - ts.x / 2, c.y + r + 4), ImGui::GetColorU32(m.here ? ImGuiCol_Text : ImGuiCol_TextDisabled), name.c_str());
		}
		const float dx = io.MousePos.x - c.x, dy = io.MousePos.y - c.y;
		if (hovered && dx * dx + dy * dy <= r * r) {
			hovered_norn = k;
		}
	}
	draw->PopClipRect();

	if (!hovered_norn.empty()) {
		const Member& m = s_members[hovered_norn];
		auto parent = [](const std::string& p) { return is_member(p) ? label(s_members[p]) : is_founder(p) ? std::string("the Hatchery") : p; };
		ImGui::SetTooltip("%s (%s), %s%s\nMother: %s\nFather: %s\nChildren: %zu", label(m).c_str(), m.moniker.c_str(), m.sex == 2 ? "female" : "male",
			m.dead ? ", died" : m.here ? "" : ", not in this world", parent(m.mother).c_str(), parent(m.father).c_str(), children_of(hovered_norn).size());
		// a click (not a drag) chooses it; choosing it again selects it in the world
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).x + ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).y < 16) {
			if (s_focus == hovered_norn && m.here) {
				for (auto* obj : *g_engine_context.objects) {
					if (auto* c = obj->as_creature(); c && c->moniker == hovered_norn) {
						g_engine_context.world->current_norn = c->uid;
					}
				}
			}
			s_focus = hovered_norn;
		}
	}
}

} // namespace

const Member* find(const std::string& moniker) {
	auto it = s_members.find(moniker);
	return it != s_members.end() ? &it->second : nullptr;
}

int generation_of(const std::string& moniker) {
	if (!s_loaded) {
		load();
	}
	if (!is_member(moniker)) {
		return 0;
	}
	std::map<std::string, int> memo;
	return depth_of(moniker, memo) + 1;
}

bool name_taken(const std::string& name) {
	if (!s_loaded) {
		load();
	}
	for (auto& [k, m] : s_members) {
		if (m.name == name) {
			return true;
		}
	}
	return false;
}

void world_changed() {
	load();
	Wolfling::set_family_lookup([](const std::string& moniker) { const Member* m = find(moniker); return m ? m->name : std::string(); },
		[](const std::string& name) { return name_taken(name); });
}

void tick() {
	if (!s_loaded) {
		load();
	}
	const uint32_t t = g_engine_context.world->tick;
	if (t % 50 == 0) {
		update_registry();
		automatic_photos();
	}
	if (s_changed && t % 600 == 0) {
		save();
	}
}

void open_tree() {
	s_show_tree = true;
	auto* obj = g_engine_context.objects->try_get(g_engine_context.world->current_norn);
	if (auto* c = obj ? obj->as_creature() : nullptr) {
		s_focus = c->moniker;
	}
}

void draw_windows() {
	if (!s_show_tree) {
		return;
	}
	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowSize(ImVec2(std::min(860.f, screen.x - 40), std::min(580.f, screen.y - 80)), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(screen.x / 2, screen.y / 2), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	if (ImGui::Begin("Family Tree", &s_show_tree)) {
		const Member* me = find(s_focus);
		const float button = ImGui::GetFrameHeight() * 1.2f;
		if (ImGui::Button("-##treeout", ImVec2(button, 0))) {
			s_zoom = std::max(0.15f, s_zoom / 1.25f);
		}
		ImGui::SetItemTooltip("Zoom out (or the wheel, or pinch)");
		ImGui::SameLine();
		if (ImGui::Button("+##treein", ImVec2(button, 0))) {
			s_zoom = std::min(3.f, s_zoom * 1.25f);
		}
		ImGui::SetItemTooltip("Zoom in");
		ImGui::SameLine();
		if (ImGui::Button("Fit")) {
			s_fit_next = true;
		}
		ImGui::SetItemTooltip("Show the whole family");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(std::min(260.f, ImGui::GetContentRegionAvail().x * 0.5f));
		if (ImGui::BeginCombo("##findnorn", me ? label(*me).c_str() : "Find a Norn")) {
			for (auto& [k, m] : s_members) {
				if (ImGui::Selectable(fmt::format("{} ({}){}", label(m), m.moniker, m.dead ? ", died" : "").c_str(), k == s_focus)) {
					s_focus = k;
					// bring it to the middle of the view
					if (auto it = s_layout.at.find(k); it != s_layout.at.end()) {
						const ImVec2 avail = ImGui::GetContentRegionAvail();
						s_pan = ImVec2(it->second.x - avail.x / 2 / s_zoom, it->second.y - avail.y / 2 / s_zoom);
					}
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%zu Norns. Drag to move; pink ring female, blue male.", s_members.size());
		ImGui::Separator();
		draw_tree();
	}
	ImGui::End();
}

} // namespace Family
