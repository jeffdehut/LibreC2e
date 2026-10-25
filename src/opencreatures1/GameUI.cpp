#include "common/SessionProfile.h"
#include "GameUI.h"
#include "GameText.h"
#include "CobInjector.h"
#include "sdlbackend/SDLBackend.h"
#include "sdlbackend/SDLTouch.h"

#include "CreatureBiochemistry.h"
#include "CreatureGenetics.h"
#include "EngineContext.h"
#include "Kits.h"
#include "MacroManager.h"
#include "MapManager.h"
#include "PathManager.h"
#include "PointerManager.h"
#include "CrashReport.h"
#include "FileDialogs.h"
#include "Macro.h"
#include "PhotoAlbum.h"
#include "SaveAndShare.h"
#include "UIImages.h"
#include "UIArt.h"
#include "UIStyle.h"
#include "UIText.h"
#include "DayNight.h"
#include "Recording.h"
#include "RecordingUI.h"
#include "ViewportManager.h"
#include "WorldState.h"
#include "WorldSystems.h"
#include "Wolfling.h"
#include "Family.h"
#include "EggFiles.h"
#include "HdArt.h"
#include "common/Ascii.h"
#include "objects/Blackboard.h"
#include "objects/Bubble.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"
#include "objects/PointerTool.h"
#include "objects/Vehicle.h"
#include "objects/ObjectNames.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <fmt/core.h>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <set>
#include <sstream>
#include <vector>
#include <ctime>
#include <chrono>

namespace fs = std::filesystem;

namespace GameUI {

#ifdef OPENC1_LOCAL_FEATURES
void local_frame();
#endif

namespace {
bool s_hand_typing = false; // typing straight to the Norns (see type_into_hand)
bool s_touch_typing_requested = false; // a double tap on the world (touch screens)
// the modern bar's trays (draw_modern_bar)
enum class Tray { None, Give, Heal, Teach };
Tray s_tray = Tray::None;
float s_tray_x = 0; // the middle of the slot the tray opened from
float s_tray_bottom = 0;
void plushie_toggle(float size);
std::string s_hand_text;

Options s_options;
bool s_show_crash_window = false;
bool s_show_debug_console = false;

// Creature's View's eye on the world (batch 4, Q11)
constexpr int32_t EYE_WIDTH = 128;
constexpr int32_t EYE_HEIGHT = 96;
float s_eye_x = 0, s_eye_y = 0;
bool s_eye_placed = false;
ObjectHandle s_eye_creature;
bool s_paused = false;
bool s_native_drag_active = false;
bool s_quit = false;
std::string s_world_to_load;
std::string s_new_world_name; // the file name for a world started with World > New

// dialogs
bool s_show_preferences = false;
bool s_show_tip = false;
std::string s_test_popup; // debugging: a popup to open where its button is (OPENCREATURES1_TEST_DIALOG)
bool s_show_about = false;
bool s_show_help = false;
bool s_show_add_place = false;
bool s_show_remove_place = false;
bool s_show_open = false;
bool s_show_save_as = false;
bool s_show_new = false;
std::string s_message; // a message box's text, shown while not empty

std::vector<std::string> s_tips;
size_t s_tip_index = 0;

char s_sentence[64] = "";
std::string s_last_sentence;
double s_which_is_my_norn_until = 0;

// the status bar's health, refreshed every 100 ticks (Q1)
constexpr uint32_t HEALTH_REFRESH_TICKS = 100;
int32_t s_health = -1;
uint32_t s_health_tick = 0;

// the hand's words show for 10 ticks; the last 10 sentences are kept (batch 4, Q23, Q25)
constexpr int32_t HAND_BUBBLE_TICKS = 10;
constexpr size_t SPEECH_HISTORY = 10;

fs::path options_path() {
	if (auto profile = session_profile(); !profile.empty()) { return profile / "options.ini"; }
	char* dir = SDL_GetPrefPath("openc2e", "opencreatures1");
	if (!dir) {
		return {};
	}
	fs::path path = fs::path(dir) / "options.ini";
	SDL_free(dir);
	return path;
}

Rect2f to_rectf(const Rect2i& r) {
	return Rect2f{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.width), static_cast<float>(r.height)};
}

// the centre of the view, in world coordinates
std::pair<float, float> view_centre() {
	auto r = to_rectf(g_engine_context.viewport->get_main_camera_rect());
	return {r.x + r.width / 2, r.y + r.height / 2};
}

Creature* selected_creature() {
	auto* obj = g_engine_context.objects->try_get(g_engine_context.world->current_norn);
	return obj ? obj->as_creature() : nullptr;
}

std::string creature_label(const Creature& c) {
	std::string name = c.history_name.empty() || c.history_name == "<Un Named>" ? c.moniker : c.history_name;
	const char* state = c.dead ? "(Deceased)" : c.sex == 2 ? "(Female)" : "(Male)";
	return fmt::format("{} {}", name, state);
}

void select_creature(Creature& c) {
	g_engine_context.world->current_norn = c.uid;
	auto b = c.get_bbox();
	g_engine_context.viewport->centre_on(b.x + b.width / 2, b.y + b.height / 2);
}

// While the player types, a thought cloud by the hand shows the text (batch 4, Q25).
void update_hand_thought(const std::string& text) {
	auto* hand = g_engine_context.objects->try_get(g_engine_context.pointer->m_pointer_tool);
	auto* tool = hand ? hand->as_pointer_tool() : nullptr;
	if (!tool) {
		return;
	}
	auto* obj = g_engine_context.objects->try_get(tool->bubble);
	auto* cloud = obj ? obj->as_bubble() : nullptr;
	if (text.empty()) {
		if (cloud) {
			g_engine_context.objects->destroy_later(cloud->uid);
		}
		tool->bubble = {};
		tool->text.clear();
		return;
	}
	if (!cloud) {
		cloud = make_speech_bubble(*hand, text, 0, true);
		tool->bubble = cloud->uid;
	}
	cloud->text = text.substr(0, BUBBLE_MAX_TEXT);
	tool->text = cloud->text;
}

void say(const std::string& sentence) {
	// the hand's words appear in a bubble and every creature that can hear the hand hears
	// them (Q7; batch 2, Q22)
	auto* hand = g_engine_context.objects->try_get(g_engine_context.pointer->m_pointer_tool);
	if (!hand || sentence.empty()) {
		return;
	}
	update_hand_thought("");
	make_speech_bubble(*hand, sentence, HAND_BUBBLE_TICKS);
	// A listener can create a reply bubble while learning. Keep membership stable.
	std::vector<ObjectHandle> listeners;
	for (auto* obj : *g_engine_context.objects)
		if (obj->as_creature()) listeners.push_back(obj->uid);
	for (auto handle : listeners) {
		auto* obj = g_engine_context.objects->try_get(handle);
		if (auto* creature = obj ? obj->as_creature() : nullptr; creature && creature->can_hear(*hand))
			creature->hear_sentence(sentence, hand);
	}
	s_last_sentence = sentence;
	auto& history = g_engine_context.world->speech_history;
	history.push_back(sentence);
	history.erase(std::remove(history.begin(), history.end() - 1, sentence), history.end() - 1);
	while (history.size() > SPEECH_HISTORY) {
		history.erase(history.begin());
	}
}

void instant_verb_vocabulary() {
	if (auto* c = selected_creature(); c && !c->dead) {
		const auto count = c->learn_action_words();
		show_message(fmt::format("{} learned {} action words, including push, pull, get and drop.",
			c->history_name.empty() || c->history_name == "<Un Named>" ? c->moniker : c->history_name, count));
	} else {
		show_message("Select a living Norn to teach action words.");
	}
}

void add_favourite_place(const std::string& name) {
	// the six favourite places are kept in the world file (Q5)
	auto& camera = g_engine_context.viewport->get_main_camera_rect();
	for (auto& place : g_engine_context.world->favorite_places) {
		if (place.name.empty()) {
			place.name = name;
			place.x = static_cast<int16_t>(camera.x);
			place.y = static_cast<int16_t>(camera.y);
			return;
		}
	}
	s_message = "There are already six favourite places. Remove one first.";
}

std::vector<std::string> world_files() {
	return list_world_files();
}


void load_tips() {
	// Tips.txt: one tip per line (Q9)
	s_tips.clear();
	auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "Tips.txt");
	if (path.empty()) {
		return;
	}
	std::ifstream in(path, std::ios::binary);
	std::string line;
	while (std::getline(in, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
			line.pop_back();
		}
		if (!line.empty()) {
			// the file is Windows-1252; keep plain ASCII so ImGui's font can show it
			std::string ascii;
			for (char ch : line) {
				ascii += static_cast<unsigned char>(ch) < 128 ? ch : '\'';
			}
			s_tips.push_back(ascii);
		}
	}
}

void open_tip() {
	if (s_tips.empty()) {
		load_tips();
	}
	if (s_tips.empty()) {
		s_message = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "Tips.txt").empty()
						? GameText::line(GameText::Original{52, 0x55d6f7f6faf7bf2dull}, "There's no tips file in the game folder.")
						: GameText::line(GameText::Original{29, 0x904efce723ffade8ull}, "The tips file couldn't be read.");
		return;
	}
	s_show_tip = true;
}

bool save_world_now() {
	try {
		Kits::flush_records();
		save_world(get_world_file_name());
		fmt::print("* Saved world to: {}\n", get_world_file_name());
		show_message("Saved " + fs::path(get_world_file_name()).stem().string());
		return true;
	} catch (std::exception& e) {
		s_message = fmt::format("Couldn't save the world as {}: {}", get_world_file_name(), e.what());
		return false;
	}
}

void create_test_norn(int32_t sex) {
	// Testing > Create a norn: a child of the starter genomes, in the middle of the view
	std::string moniker = make_child_genome("dad1", "mum1");
	if (moniker.empty()) {
		s_message = "Couldn't find the dad1 and mum1 genomes";
		return;
	}
	auto handle = create_creature(moniker, sex);
	auto* c = g_engine_context.objects->try_get(handle)->as_creature();
	auto [x, y] = view_centre();
	c->move_ground_to(x, y);
	c->land();
	g_engine_context.macros->queue_script(c, c, SCRIPT_INITIALIZE);
	select_creature(*c);
}

// --- the menu bar (Q2) ---

void toast(std::string text, ObjectHandle object = {}, bool error = false, std::string details = "");
void draw_avatar(ImDrawList* draw, const Creature& c, ImVec2 centre, float radius, bool selected, int generation = 0);

// --- importing and exporting creatures (batch 6, Q12) ---
// Classic mode copies the original: one file at a time, its dialog titles, the creature put
// at (2591, 927), the file deleted afterwards, and Import greyed while paused or with 8 or more
// Norns. Modern mode imports several files at once, where the player is looking (or where
// they're dropped on the window), and keeps the files.

const std::vector<FileDialogs::Filter> EXP_FILTERS = {{"Export Files (*.exp)", "exp"}, {"All Files (*.*)", "*"}};
std::string s_creature_folder; // the folder last used for importing or exporting

int norn_count() {
	int n = 0;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature(); c && c->genus == 1) {
			n++;
		}
	}
	return n;
}

bool can_import() {
	return !s_options.classic_layout || (!paused() && norn_count() < 8);
}

bool can_export(const Creature* c) {
	return c && !c->dead && (!s_options.classic_layout || !paused());
}

std::string creature_folder() {
	if (!s_creature_folder.empty()) {
		return s_creature_folder;
	}
	if (!s_options.classic_layout) {
		// the modern layout keeps a world's Norns and eggs with it
		std::error_code err;
		const auto norns = current_world_folder() / "Norns";
		std::filesystem::create_directories(norns, err);
		if (!err) {
			return norns.string();
		}
	}
	return g_engine_context.paths->find_path(PATH_TYPE_MAIN, "Creatures.exe").parent_path().string();
}

void remember_folder(const std::string& file) {
	s_creature_folder = std::filesystem::path(file).parent_path().string();
}

void import_files(const std::vector<std::string>& paths, float x, float y) {
	const bool classic = s_options.classic_layout;
	for (size_t i = 0; i < paths.size(); ++i) {
		try {
			// an egg (LibreCreatures' .egg) arrives by the incubator, as the Hatchery's do
			if (EggFiles::is_egg_file(paths[i])) {
				remember_folder(paths[i]);
				const auto moniker = EggFiles::import_egg(paths[i]);
				toast(fmt::format("An egg ({}) is by the incubator", moniker));
				continue;
			}
			// several at once are spread out a little
			if (auto* c = import_creature(paths[i], x + 40.f * static_cast<float>(i), y, classic)) {
				select_creature(*c);
				toast(fmt::format("{} has arrived", creature_label(*c)), c->uid);
			}
			remember_folder(paths[i]);
		} catch (std::exception& e) {
			s_message = fmt::format("Couldn't import {}: {}", std::filesystem::path(paths[i]).filename().string(), e.what());
			fmt::print("WARN [Import] {}\n", s_message);
		}
	}
}

void choose_import_files() {
	if (s_options.classic_layout) {
		FileDialogs::open(GameText::line(GameText::Original{24, 0xbb117b3dc5b3f00dull}, "Import creatures or objects"), EXP_FILTERS, "", false, [](const std::vector<std::string>& paths) {
			import_files(paths, ORIGINAL_IMPORT_X, ORIGINAL_IMPORT_Y);
		});
		return;
	}
	FileDialogs::open("Import Eggs or Creatures", {{"Eggs and creatures (*.egg, *.exp)", "egg;exp"}, {"Eggs (*.egg)", "egg"}, {"Creatures (*.exp)", "exp"}}, creature_folder(), true, [](const std::vector<std::string>& paths) {
		auto [x, y] = view_centre();
		import_files(paths, x, y);
	});
}

// The egg the hand holds, out of the world into a LibreCreatures .egg file
void choose_export_egg() {
	auto* egg = EggFiles::held_egg();
	if (!egg) {
		return;
	}
	const ObjectHandle which = egg->uid;
	const auto generation = g_engine_context.generation();
	const auto name = moniker_from_integer(egg->obv0) + ".egg";
	FileDialogs::save("Export Held Egg", {{"Eggs (*.egg)", "egg"}}, (std::filesystem::path(creature_folder()) / name).string(), [which, generation](const std::vector<std::string>& paths) {
		if (generation != g_engine_context.generation() || paths.empty()) return;
		auto* held = EggFiles::held_egg();
		if (!held || held->uid != which) {
			s_message = "The egg has to stay in the hand until it's exported.";
			return;
		}
		std::filesystem::path path = paths[0];
		if (path.extension().empty()) {
			path += ".egg";
		}
		try {
			EggFiles::export_held_egg(path);
			remember_folder(path.string());
			toast(fmt::format("The egg was exported to {}", path.filename().string()));
		} catch (const std::exception& e) {
			s_message = fmt::format("Couldn't export the egg: {}", e.what());
		}
	});
}

void choose_import_egg() {
	FileDialogs::open("Import Egg", {{"Eggs (*.egg)", "egg"}}, creature_folder(), true, [](const std::vector<std::string>& paths) {
		auto [x, y] = view_centre();
		import_files(paths, x, y);
	});
}

void choose_export_file(Creature& c) {
	const bool classic = s_options.classic_layout;
	const std::string name = classic ? "Creature.exp" : export_file_name(c) + ".exp";
	const ObjectHandle who = c.uid;
	const auto generation = g_engine_context.generation();
	FileDialogs::save(classic ? "Export current creature" : "Export Creature", EXP_FILTERS, (std::filesystem::path(creature_folder()) / name).string(), [who, generation](const std::vector<std::string>& paths) {
		if (generation != g_engine_context.generation()) return;
		auto* obj = g_engine_context.objects->try_get(who);
		auto* creature = obj ? obj->as_creature() : nullptr;
		if (paths.empty() || !creature) {
			return;
		}
		std::filesystem::path path = paths[0];
		if (path.extension().empty()) {
			path += ".exp";
		}
		try {
			const std::string label = creature_label(*creature);
			export_creature(*creature, true, path.string());
			remember_folder(path.string());
			toast(fmt::format("{} was exported to {}", label, path.filename().string()));
		} catch (std::exception& e) {
			s_message = fmt::format("Couldn't export the creature: {}", e.what());
		}
	});
}

// Files dropped on the window (modern mode only; in classic mode nothing happens, as in the
// original). SDL reports them as events; they're handled once a frame.
struct DroppedFile {
	std::string path;
	float x, y;
	SDL_WindowID window;
};
std::mutex s_drop_mutex;
std::vector<DroppedFile> s_dropped;

bool SDLCALL watch_for_drops(void*, SDL_Event* event) {
	if (event->type == SDL_EVENT_DROP_FILE && event->drop.data) {
		std::lock_guard<std::mutex> lock(s_drop_mutex);
		s_dropped.push_back({event->drop.data, event->drop.x, event->drop.y, event->drop.windowID});
	}
	return true;
}

void handle_dropped_files() {
	static bool watching = SDL_AddEventWatch(watch_for_drops, nullptr);
	(void)watching;
	// debugging: OPENCREATURES1_TEST_MOUSE=1 sends real SDL mouse events after 3 s (a move and a
	// click on the File menu, then a move over the world) and logs what the interface and hand saw
	if (std::getenv("OPENCREATURES1_TEST_MOUSE")) {
		static int step = 0;
		static double next = 3;
		if (ImGui::GetTime() > next && step < 8) {
			int count = 0;
			SDL_Window** windows = SDL_GetWindows(&count);
			SDL_WindowID id = windows && count > 0 ? SDL_GetWindowID(windows[0]) : 0;
			SDL_free(windows);
			SDL_Event e{};
			auto send_motion = [&](float x, float y) {
				e = SDL_Event{};
				e.type = SDL_EVENT_MOUSE_MOTION;
				e.motion.windowID = id;
				e.motion.x = x;
				e.motion.y = y;
				SDL_PushEvent(&e);
			};
			auto send_button = [&](bool down, float x, float y) {
				e = SDL_Event{};
				e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
				e.button.windowID = id;
				e.button.button = SDL_BUTTON_LEFT;
				e.button.down = down;
				e.button.clicks = 1;
				e.button.x = x;
				e.button.y = y;
				SDL_PushEvent(&e);
			};
			if (step == 0) send_motion(20, 10);
			if (step == 1) send_button(true, 20, 10);
			if (step == 2) send_button(false, 20, 10);
			if (step == 3) fmt::print("TEST [Mouse] mouse at {},{}, over interface {}, a menu open {}, paused {}\n", ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, ImGui::GetIO().WantCaptureMouse, ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), paused());
			if (step == 4) send_motion(400, 300);
			if (step == 6) {
				auto* hand = g_engine_context.objects->try_get(g_engine_context.pointer->m_pointer_tool);
				fmt::print("TEST [Mouse] mouse at {},{}; hand screen {},{}; hand world {}; over interface {}; locked {}\n", ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, g_engine_context.pointer->m_screenx, g_engine_context.pointer->m_screeny,
					hand ? fmt::format("{},{}", hand->get_bbox().x, hand->get_bbox().y) : "none", g_engine_context.pointer->m_over_interface, g_engine_context.pointer->m_locked);
			}
			step++;
			next = ImGui::GetTime() + 0.3;
		}
	}
	// debugging: OPENCREATURES1_TEST_NORN_DROP=1 picks up the first Norn after 3 s, as Shift+click
	// does, lifts it 300 px and 400 px to the right, lets go, and logs where it lands
	if (std::getenv("OPENCREATURES1_TEST_NORN_DROP")) {
		static int step = 0;
		static double next = 3;
		static ObjectHandle who;
		if (ImGui::GetTime() > next && step < 3) {
			auto pointer = g_engine_context.pointer->m_pointer_tool;
			if (step == 0) {
				for (auto* obj : *g_engine_context.objects) {
					if (auto* c = obj->as_creature(); c && c->genus == 1 && !c->dead) {
						who = c->uid;
						g_engine_context.messages->hand_message(pointer, c->uid, MESSAGE_PICKUP);
						fmt::print("TEST [Norn drop] picked up {}: carried {}\n", c->moniker, c->carrier == pointer);
						break;
					}
				}
			} else if (step == 1) {
				if (auto* c = g_engine_context.objects->try_get(who) ? g_engine_context.objects->try_get(who)->as_creature() : nullptr) {
					c->move_by(400, -300);
					g_engine_context.messages->hand_message(pointer, c->uid, MESSAGE_DROP);
				}
			} else if (auto* obj = g_engine_context.objects->try_get(who)) {
				auto* c = obj->as_creature();
				const Room* room = g_engine_context.map->find_room_containing(c->ground_x(), c->ground_y() - 1);
				fmt::print("TEST [Norn drop] {} landed at {},{}; limits bottom {}; room under its feet {}\n", c->moniker, c->ground_x(), c->ground_y(), c->limit.bottom(),
					room ? fmt::format("{},{}-{},{}", room->left, room->top, room->right, room->bottom) : std::string("none"));
			}
			step++;
			next = ImGui::GetTime() + 1;
		}
	}
	// debugging: OPENCREATURES1_TEST_VENDOR=1 centres the view on the island carrot vendor after
	// 3 s, has the hand push it, and a second later logs every carrot near it
	if (std::getenv("OPENCREATURES1_TEST_VENDOR")) {
		static int step = 0;
		static double next = 3;
		if (ImGui::GetTime() > next && step < 20) {
			for (auto* obj : *g_engine_context.objects) {
				if (obj->family != 2 || obj->genus != 8 || obj->species != 3) {
					continue;
				}
				const auto b = obj->get_bbox();
				if (step == 0) {
					s_options.track_norn = false;
					s_show_tip = false;
					fmt::print("TEST [Vendor] vendor at {},{} plane {}\n", b.x, b.y, obj->get_z_order());
					g_engine_context.viewport->set_scroll_position(static_cast<int32_t>(b.x) - 300, static_cast<int32_t>(b.y) - 200);
					g_engine_context.viewport->update();
				} else if (step <= 3) {
					// a real move, press and release, as the player makes them, so the hand's
					// own script runs too
					int count = 0;
					SDL_Window** windows = SDL_GetWindows(&count);
					const SDL_WindowID id = windows && count > 0 ? SDL_GetWindowID(windows[0]) : 0;
					SDL_free(windows);
					const float x = g_engine_context.viewport->world_x_to_window_x(b.x + 50) / ImGui::GetIO().DisplayFramebufferScale.x;
					const float y = g_engine_context.viewport->world_y_to_window_y(b.y + 70) / ImGui::GetIO().DisplayFramebufferScale.y;
					SDL_Event e{};
					if (step == 1) {
						e.type = SDL_EVENT_MOUSE_MOTION;
						e.motion.windowID = id;
						e.motion.x = x;
						e.motion.y = y;
					} else {
						e.type = step == 2 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
						e.button.windowID = id;
						e.button.button = SDL_BUTTON_LEFT;
						e.button.down = step == 2;
						e.button.clicks = 1;
						e.button.x = x;
						e.button.y = y;
					}
					SDL_PushEvent(&e);
					fmt::print("TEST [Vendor] step {} at {},{}: hand locked {}, over interface {}\n", step, x, y, g_engine_context.pointer->m_locked, g_engine_context.pointer->m_over_interface);
				} else {
					for (auto* o : *g_engine_context.objects) {
						const auto c = o->get_bbox();
						if (o->family == 2 && o->genus == 6 && o->species == 3 && std::abs(c.x - b.x) < 400) {
							fmt::print("TEST [Vendor] carrot at {},{} size {}x{} pose {} attr {} plane {} limit {},{}-{},{}\n", c.x, c.y, c.width, c.height,
								o->get_part(0) ? o->get_part(0)->pose() : -1, o->attr, o->get_z_order(), o->limit.x, o->limit.y, o->limit.right(), o->limit.bottom());
						}
					}
				}
			}
			step++;
			next = ImGui::GetTime() + 1;
		}
	}
	// debugging: OPENCREATURES1_TEST_TOUCH_SCROLL=1 drags a finger up over the middle of the
	// Give tray (open it with OPENCREATURES1_TEST_TRAY=give) and logs how far it scrolled and
	// whether anything was given (it shouldn't be: the drag isn't a tap)
	if (std::getenv("OPENCREATURES1_TEST_TOUCH_SCROLL")) {
		static int step = 0;
		static double next = 3;
		if (ImGui::GetTime() > next && step < 14) {
			next = ImGui::GetTime() + 0.1;
			int count = 0;
			SDL_Window** windows = SDL_GetWindows(&count);
			SDL_Window* window = windows && count > 0 ? windows[0] : nullptr;
			SDL_free(windows);
			int ww = 0, wh = 0;
			SDL_GetWindowSize(window, &ww, &wh);
			ImGuiWindow* tray = ImGui::FindWindowByName("##givetray");
			const float x = tray ? tray->Pos.x + tray->Size.x / 2 : static_cast<float>(ww) / 2;
			const float y0 = tray ? tray->Pos.y + tray->Size.y * 0.7f : static_cast<float>(wh) / 2;
			auto finger = [&](SDL_EventType type, float y) {
				SDL_Event e{};
				e.type = type;
				e.tfinger.touchID = 77;
				e.tfinger.fingerID = 9;
				e.tfinger.windowID = window ? SDL_GetWindowID(window) : 0;
				e.tfinger.x = x / static_cast<float>(ww);
				e.tfinger.y = y / static_cast<float>(wh);
				SDL_PushEvent(&e);
			};
			static float y = 0;
			static int objects_before = 0;
			auto objects = [] {
				int n = 0;
				for (auto* o : *g_engine_context.objects) {
					(void)o;
					++n;
				}
				return n;
			};
			if (step == 0) {
				y = y0;
				objects_before = objects();
				finger(SDL_EVENT_FINGER_DOWN, y);
			} else if (step < 12) {
				y -= 18;
				finger(SDL_EVENT_FINGER_MOTION, y);
			} else if (step == 12) {
				finger(SDL_EVENT_FINGER_UP, y);
			} else {
				fmt::print("TEST touch scroll: the tray scrolled {:.0f} of {:.0f}; {} objects added (none if the drag wasn't taken for a tap)\n", tray ? tray->Scroll.y : -1.f, tray ? tray->ScrollMax.y : -1.f, objects() - objects_before);
				std::fflush(stdout);
			}
			++step;
		}
	}
	// debugging: OPENCREATURES1_TEST_CAMERA=1 sends, half a second apart, a middle-button drag,
	// a wheel scroll, a trackpad pinch, then touch-screen gestures (two-finger drag and pinch, a
	// tap, a hold, a double tap), and logs the camera and the hand after each
	if (std::getenv("OPENCREATURES1_TEST_CAMERA")) {
		static int step = 0;
		static double next = 3;
		if (ImGui::GetTime() > next && step < 40) {
			int count = 0;
			SDL_Window** windows = SDL_GetWindows(&count);
			const SDL_WindowID id = windows && count > 0 ? SDL_GetWindowID(windows[0]) : 0;
			SDL_Window* window = windows && count > 0 ? windows[0] : nullptr;
			SDL_free(windows);
			int ww = 0, wh = 0;
			SDL_GetWindowSize(window, &ww, &wh);
			const float cx = static_cast<float>(ww) / 2, cy = static_cast<float>(wh) / 3;
			auto motion = [&](float x, float y, Uint32 state) {
				SDL_Event e{};
				e.type = SDL_EVENT_MOUSE_MOTION;
				e.motion.windowID = id;
				e.motion.x = x;
				e.motion.y = y;
				e.motion.state = state;
				SDL_PushEvent(&e);
			};
			auto button = [&](Uint8 b, bool down, float x, float y) {
				SDL_Event e{};
				e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
				e.button.windowID = id;
				e.button.button = b;
				e.button.down = down;
				e.button.clicks = 1;
				e.button.x = x;
				e.button.y = y;
				SDL_PushEvent(&e);
			};
			auto finger = [&](SDL_EventType type, SDL_FingerID f, float x, float y) {
				SDL_Event e{};
				e.type = type;
				e.tfinger.touchID = 77;
				e.tfinger.fingerID = f;
				e.tfinger.windowID = id;
				e.tfinger.x = x / static_cast<float>(ww);
				e.tfinger.y = y / static_cast<float>(wh);
				SDL_PushEvent(&e);
			};
			const char* what = "";
			switch (step) {
				case 0: s_options.track_norn = false; s_show_tip = false; what = "start"; break;
				case 1: motion(cx, cy, 0); button(SDL_BUTTON_MIDDLE, true, cx, cy); what = "middle down"; break;
				case 2: case 3: case 4: case 5: motion(cx - 40.f * static_cast<float>(step - 1), cy, SDL_BUTTON_MMASK); what = "middle drag 40 px left"; break;
				case 6: button(SDL_BUTTON_MIDDLE, false, cx - 160, cy); what = "middle up (fling)"; break;
				case 7: what = "after fling"; break;
				case 8: { SDL_Event e{}; e.type = SDL_EVENT_MOUSE_WHEEL; e.wheel.windowID = id; e.wheel.y = -3; e.wheel.mouse_x = cx; e.wheel.mouse_y = cy; SDL_PushEvent(&e); what = "wheel 3 down"; break; }
				case 9: what = "after wheel"; break;
				case 10: { SDL_Event e{}; e.type = SDL_EVENT_PINCH_UPDATE; e.pinch.windowID = id; e.pinch.scale = 1.5f; SDL_PushEvent(&e); what = "trackpad pinch x1.5"; break; }
				case 11: { SDL_Event e{}; e.type = SDL_EVENT_PINCH_UPDATE; e.pinch.windowID = id; e.pinch.scale = 1 / 1.5f; SDL_PushEvent(&e); what = "trackpad pinch back"; break; }
				case 12: finger(SDL_EVENT_FINGER_DOWN, 1, cx - 50, cy); finger(SDL_EVENT_FINGER_DOWN, 2, cx + 50, cy); what = "two fingers down"; break;
				case 13: finger(SDL_EVENT_FINGER_MOTION, 1, cx - 150, cy); finger(SDL_EVENT_FINGER_MOTION, 2, cx + 150, cy); what = "spread to 300 px apart (zoom in)"; break;
				case 14: finger(SDL_EVENT_FINGER_MOTION, 1, cx - 50, cy); finger(SDL_EVENT_FINGER_MOTION, 2, cx + 50, cy); what = "back to 100 px (zoom out)"; break;
				case 15: finger(SDL_EVENT_FINGER_MOTION, 1, cx - 50, cy + 100); finger(SDL_EVENT_FINGER_MOTION, 2, cx + 50, cy + 100); what = "both 100 px down (pan)"; break;
				case 16: finger(SDL_EVENT_FINGER_UP, 1, cx - 50, cy + 100); finger(SDL_EVENT_FINGER_UP, 2, cx + 50, cy + 100); what = "two fingers up"; break;
				case 17: finger(SDL_EVENT_FINGER_DOWN, 3, cx, cy); what = "tap down"; break;
				case 18: finger(SDL_EVENT_FINGER_UP, 3, cx, cy); what = "tap up (left click)"; break;
				case 20: finger(SDL_EVENT_FINGER_DOWN, 4, cx + 30, cy); what = "hold down"; break;
				case 22: finger(SDL_EVENT_FINGER_UP, 4, cx + 30, cy); what = "hold up (right click after 450 ms)"; break;
				case 24: finger(SDL_EVENT_FINGER_DOWN, 5, cx, cy); finger(SDL_EVENT_FINGER_UP, 5, cx, cy); what = "double tap 1"; break;
				case 25: finger(SDL_EVENT_FINGER_DOWN, 6, cx, cy); finger(SDL_EVENT_FINGER_UP, 6, cx, cy); what = "double tap 2"; break;
				case 26: what = "after double tap"; break;
				default: break;
			}
			const auto& cam = g_engine_context.viewport->get_main_camera_rect();
			auto* hand = g_engine_context.objects->try_get(g_engine_context.pointer->m_pointer_tool);
			fmt::print("TEST [Camera] {:2} {}: camera {},{} {}x{} scale {:.2f}; hand at {:.0f},{:.0f}; typing {}\n", step, what, cam.x, cam.y, cam.width, cam.height,
				g_engine_context.viewport->scale, hand ? hand->get_bbox().x : 0.f, hand ? hand->get_bbox().y : 0.f, s_hand_typing);
			step++;
			// the steps in the middle of the drag follow quickly, as a real drag does
			next = ImGui::GetTime() + (step >= 2 && step <= 6 ? 0.03 : (step == 18 ? 0.1 : step == 20 || step == 24 ? 0.6 : step == 25 ? 0.15 : 0.5));
		}
	}
	// debugging: OPENCREATURES1_TEST_CARRY=<family genus species> moves the hand to the middle of
	// the view after 3 s and puts the first such object (a carrot, 2 6 3, by default) in it
	if (const char* carry = std::getenv("OPENCREATURES1_TEST_CARRY")) {
		static int step = 0;
		static double next = 3;
		if (ImGui::GetTime() > next && step < 3) {
			int f = 2, g = 6, sp = 3;
			std::sscanf(carry, "%d %d %d", &f, &g, &sp);
			int count = 0;
			SDL_Window** windows = SDL_GetWindows(&count);
			SDL_Event e{};
			e.type = SDL_EVENT_MOUSE_MOTION;
			e.motion.windowID = windows && count > 0 ? SDL_GetWindowID(windows[0]) : 0;
			int ww = 0, wh = 0;
			SDL_GetWindowSize(windows && count > 0 ? windows[0] : nullptr, &ww, &wh);
			SDL_free(windows);
			e.motion.x = static_cast<float>(ww) / 2;
			e.motion.y = static_cast<float>(wh) / 3;
			if (step == 0) {
				s_options.track_norn = false;
				s_show_tip = false;
				SDL_PushEvent(&e);
			} else if (step == 1) {
				for (auto* obj : *g_engine_context.objects) {
					if (obj->family == f && obj->genus == g && obj->species == sp && !(obj->attr & ATTR_INVISIBLE)) {
						g_engine_context.messages->hand_message(g_engine_context.pointer->m_pointer_tool, obj->uid, MESSAGE_PICKUP);
						g_engine_context.pointer->run_hand_script(obj, SCRIPT_POINTER_PICKUP);
						break;
					}
				}
			} else {
				auto* hand = g_engine_context.objects->try_get(g_engine_context.pointer->m_pointer_tool);
				if (auto* held = g_engine_context.objects->try_get(g_engine_context.pointer->m_carried_object); held && hand) {
					fmt::print("TEST [Carry] hand at {},{} plane {}; held {} at {},{} size {}x{} plane {}\n", hand->get_bbox().x, hand->get_bbox().y, hand->get_z_order(),
						fmt::format("{} {} {}", held->family, held->genus, held->species), held->get_bbox().x, held->get_bbox().y, held->get_bbox().width, held->get_bbox().height, held->get_z_order());
				}
			}
			step++;
			next = ImGui::GetTime() + 1;
		}
	}
	// debugging: OPENCREATURES1_TEST_NAMES=<n> logs n wolfling names for each of G1 to G4;
	// OPENCREATURES1_TEST_TREE=1 opens the family tree
	if (const char* names = std::getenv("OPENCREATURES1_TEST_NAMES")) {
		static bool done = false;
		if (!done && ImGui::GetTime() > 2) {
			done = true;
			for (int g = 1; g <= 4; ++g) {
				std::string line;
				for (auto& n : Wolfling::sample_names(g, std::atoi(names))) {
					line += n + " ";
				}
				fmt::print("TEST [Names] G{}: {}\n", g, line);
			}
		}
	}
	if (std::getenv("OPENCREATURES1_TEST_TREE")) {
		static bool done = false;
		if (!done && ImGui::GetTime() > 2) {
			done = true;
			s_show_tip = false;
			Family::open_tree();
		}
	}
	// debugging: OPENCREATURES1_TEST_DIALOG=settings|help|load|new|saveas|tip|place|world|more
	// opens a dialog (or the folded world menu, or the bar's More menu) after 2 s
	if (const char* dialog = std::getenv("OPENCREATURES1_TEST_DIALOG")) {
		static bool done = false;
		if (!done && ImGui::GetTime() > 2) {
			done = true;
			s_show_tip = false;
			const std::string d = dialog;
			s_show_preferences = d == "settings";
			s_show_help = d == "help";
			s_show_open = d == "load";
			s_show_new = d == "new";
			s_show_save_as = d == "saveas";
			s_show_add_place = d == "place";
			if (d == "tip") {
				open_tip();
			}
			s_test_popup = d == "world" ? "##worldmenu" : d == "more" ? "##moremenu" : "";
		}
	}
	// debugging: OPENCREATURES1_TEST_TRAY=give|heal|teach opens a tray of the modern bar, and
	// OPENCREATURES1_TEST_PANE=<norn|lab|objects|records>:<section> a pane, after 2 s
	if (const char* tray = std::getenv("OPENCREATURES1_TEST_TRAY")) {
		static bool done = false;
		if (!done && ImGui::GetTime() > 2) {
			done = true;
			s_show_tip = false;
			const std::string t = tray;
			s_tray = t == "give" ? Tray::Give : t == "heal" ? Tray::Heal : t == "teach" ? Tray::Teach : Tray::None;
			s_tray_x = ImGui::GetIO().DisplaySize.x / 2;
		}
	}
	if (const char* pane = std::getenv("OPENCREATURES1_TEST_PANE")) {
		static bool done = false;
		if (!done && ImGui::GetTime() > 2) {
			done = true;
			s_show_tip = false;
			const std::string p = pane;
			const std::string name = p.substr(0, p.find(':'));
			const int section = p.find(':') == std::string::npos ? 0 : std::atoi(p.c_str() + p.find(':') + 1);
			Kits::open_pane(name == "norn" ? Kits::Pane::Norn : name == "lab" ? Kits::Pane::Lab : name == "objects" ? Kits::Pane::Objects : Kits::Pane::Records, section);
		}
	}
	// debugging: OPENCREATURES1_TEST_GIVE=<part of a name> gives the first Give-tray object whose
	// name has that in it to the selected Norn after 3 s, and logs where things are 3 s later
	if (const char* give = std::getenv("OPENCREATURES1_TEST_GIVE")) {
		static int step = 0;
		static double next = 3;
		if (ImGui::GetTime() > next && step < 2) {
			auto* c = selected_creature();
			if (step == 0) {
				for (const auto& item : Kits::injector_items(true)) {
					if (item.name.find(give) != std::string::npos) {
						const bool ok = Kits::add_item(item);
						fmt::print("TEST [Give] gave {:?}: {}\n", item.name, ok);
						break;
					}
				}
			} else if (c) {
				fmt::print("TEST [Give] {} stands at {:.0f},{:.0f}; room {}-{}\n", c->moniker, c->ground_x(), c->ground_y(), c->limit.x, c->limit.right());
				auto* hand = g_engine_context.objects->try_get(g_engine_context.pointer->m_pointer_tool);
				fmt::print("TEST [Give] the hand holds {}\n", g_engine_context.objects->try_get(g_engine_context.pointer->m_edited_object) || g_engine_context.objects->try_get(g_engine_context.pointer->m_carried_object) ? "something" : "nothing");
				(void)hand;
			}
			step++;
			next = ImGui::GetTime() + 3;
		}
	}
	// debugging: OPENCREATURES1_TEST_HAND_TYPING=<words> presses Enter, types the words and presses
	// Enter again after 3 s, as a player typing straight to the Norns would
	if (const char* words = std::getenv("OPENCREATURES1_TEST_HAND_TYPING")) {
		static int step = 0;
		ImGuiIO& io = ImGui::GetIO();
		if (ImGui::GetTime() > 3 && step < 6) {
			if (step == 0 || step == 4) {
				io.AddKeyEvent(ImGuiKey_Enter, true);
			} else if (step == 1 || step == 5) {
				io.AddKeyEvent(ImGuiKey_Enter, false);
			} else if (step == 2) {
				io.AddInputCharactersUTF8(words);
			} else if (step == 3) {
				fmt::print("TEST [Hand typing] the cloud shows \"{}\", typing {}\n", s_hand_text, s_hand_typing);
			}
			step++;
		}
	}
	// debugging: OPENCREATURES1_TEST_INCUBATOR=1 puts a Hatchery egg in the hand after 3 s,
	// drops it into the incubator a second later, and logs the door's frame for a while
	if (std::getenv("OPENCREATURES1_TEST_INCUBATOR")) {
		static int stage = 0;
		static ObjectHandle incubator;
		static double next = 3;
		Object* inc = nullptr;
		for (auto* obj : *g_engine_context.objects) {
			if (obj->family == 3 && obj->genus == 1 && obj->species == 3) {
				inc = obj;
				incubator = obj->uid;
			}
		}
		if (inc && ImGui::GetTime() > next) {
			auto* pointer = g_engine_context.objects->try_get(g_engine_context.pointer->m_pointer_tool);
			if (stage == 0) {
				auto b = inc->get_bbox();
				g_engine_context.viewport->centre_on(b.x + b.width / 2, b.y + b.height / 2);
				if (auto* v = inc->as_vehicle()) {
					fmt::print("TEST [Incubator] at {},{} size {}x{}, cabin {},{} {}x{}\n", b.x, b.y, b.width, b.height, v->cabin.x, v->cabin.y, v->cabin.width, v->cabin.height);
				}
				Macro m("inst,new: simp eggs 8 0 2000 0,pose 3,setv clas 33882624,setv attr 67,new: gene tokn mum1 tokn dad1 obv0,setv obv1 1,tick 2400,mvto 2330 800,endm");
				m.ownr = m.targ = g_engine_context.pointer->m_pointer_tool;
				g_engine_context.macros->add(m);
				stage = 1;
				next = ImGui::GetTime() + 1;
			} else if (stage == 1) {
				for (auto* obj : *g_engine_context.objects) {
					if (obj->family == 2 && obj->genus == 5 && obj->species == 2 && pointer) {
						obj->carrier = pointer->uid;
						obj->movement_status = MOVEMENT_MOUSEDRIVEN;
						obj->handle_mesg_drop(Message{pointer->uid, obj->uid, MESSAGE_DROP});
						fmt::print("TEST [Incubator] egg dropped: in the incubator {}, incubator actv {}\n", obj->carrier == incubator, static_cast<int>(inc->actv));
						break;
					}
				}
				stage = 2;
				next = ImGui::GetTime() + 0.5;
			} else if (stage < 20) {
				auto* door = inc->get_part(2);
				fmt::print("TEST [Incubator] door frame {}, actv {}\n", door ? door->frame() : -1, static_cast<int>(inc->actv));
				stage++;
				next = ImGui::GetTime() + 0.5;
			}
		}
	}
	// debugging: OPENCREATURES1_TEST_DROP=<file.exp> acts as if the file were dropped in the
	// middle of the window after 3 s
	if (const char* test = std::getenv("OPENCREATURES1_TEST_DROP")) {
		static bool done = false;
		if (!done && ImGui::GetTime() > 3) {
			done = true;
			auto* backend = dynamic_cast<SDLBackend*>(get_backend());
			static std::vector<std::string> files; // keep event data alive through SDL's queue
			std::istringstream input(test);
			std::string file;
			while (std::getline(input, file)) if (!file.empty()) files.push_back(file);
			for (const auto& path : files) {
				SDL_Event event{};
				event.type = SDL_EVENT_DROP_FILE;
				event.drop.windowID = backend ? SDL_GetWindowID(backend->window) : 0;
				event.drop.data = path.c_str();
				event.drop.x = ImGui::GetIO().DisplaySize.x / 2;
				event.drop.y = ImGui::GetIO().DisplaySize.y / 2;
				if (!SDL_PushEvent(&event)) throw std::runtime_error(SDL_GetError());
			}
		}
	}
	std::vector<DroppedFile> dropped;
	{
		std::lock_guard<std::mutex> lock(s_drop_mutex);
		dropped.swap(s_dropped);
	}
	if (s_options.classic_layout) {
		return;
	}
	std::vector<std::string> creatures;
	float x = 0, y = 0;
	auto* backend = dynamic_cast<SDLBackend*>(get_backend());
	const auto main_id = backend ? SDL_GetWindowID(backend->window) : 0;
	for (auto& d : dropped) {
		if (d.window != main_id) continue;
		const auto ext = to_ascii_lowercase(std::filesystem::path(d.path).extension().string());
		if (ext != ".exp" && ext != ".egg") {
			continue;
		}
		if (creatures.empty()) {
			x = g_engine_context.viewport->window_x_to_world_x(d.x);
			y = g_engine_context.viewport->window_y_to_world_y(d.y);
		}
		creatures.push_back(d.path);
	}
	if (!creatures.empty()) {
		import_files(creatures, x, y);
	}
}

// Option/Alt + Shift + right-drag a creature out of the window exports it where it's dropped
// (modern mode only). The creature leaves the world only once the drop has happened. Where
// the system has no drag support (Linux), leaving the window opens the Export dialog.
ObjectHandle s_drag_out;

Creature* creature_under_pointer() {
	const float wx = g_engine_context.viewport->window_x_to_world_x(g_engine_context.pointer->m_screenx);
	const float wy = g_engine_context.viewport->window_y_to_world_y(g_engine_context.pointer->m_screeny);
	Creature* found = nullptr;
	for (auto* obj : *g_engine_context.objects) {
		auto* c = obj->as_creature();
		if (c && c->get_bbox().has_point(wx, wy) && (!found || c->z_order > found->z_order)) {
			found = c;
		}
	}
	return found;
}

void handle_drag_out() {
	if (s_options.classic_layout) {
		s_drag_out = {};
		return;
	}
	const ImGuiIO& io = ImGui::GetIO();
	float gx = 0, gy = 0;
	const bool right_held = SDL_GetGlobalMouseState(&gx, &gy) & SDL_BUTTON_RMASK;
	if (!s_drag_out) {
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && io.KeyAlt && io.KeyShift && !io.WantCaptureMouse) {
			if (auto* c = creature_under_pointer(); can_export(c)) {
				s_drag_out = c->uid;
			}
		}
		return;
	}
	auto* obj = g_engine_context.objects->try_get(s_drag_out);
	auto* c = obj ? obj->as_creature() : nullptr;
	if (!right_held || !c) {
		s_drag_out = {}; // let go inside the window: nothing happens
		return;
	}
	// its face follows the pointer while it's carried to the edge
	draw_avatar(ImGui::GetForegroundDrawList(), *c, ImVec2(io.MousePos.x + 24, io.MousePos.y + 24), 22, true);
	auto* backend = dynamic_cast<SDLBackend*>(get_backend());
	SDL_Window* window = backend ? backend->window : nullptr;
	int wx = 0, wy = 0, ww = 0, wh = 0;
	if (!window || !SDL_GetWindowPosition(window, &wx, &wy) || !SDL_GetWindowSize(window, &ww, &wh)) {
		return;
	}
	const bool outside = gx < static_cast<float>(wx) || gy < static_cast<float>(wy) || gx >= static_cast<float>(wx + ww) || gy >= static_cast<float>(wy + wh);
	if (!outside) {
		return;
	}
	s_drag_out = {};
	if (!DragOut::supported()) {
		choose_export_file(*c);
		return;
	}
	try {
		const auto profile = options_path().parent_path();
		if (profile.empty()) throw std::runtime_error("Could not find the export recovery folder");
		auto transfer = std::make_shared<CreatureExportTransfer>(*c, profile / "Export Recovery");
		fmt::print("INFO [Export] Persistent recovery copy: {}\n", transfer->file().string());
		const auto label = creature_label(*c);
		s_native_drag_active = true; // keep the offered snapshot stable until drop or cancel
		DragOut::start(transfer->file().string(), [transfer, label](DragOut::Result result) {
			s_native_drag_active = false;
			if (transfer->complete(result.saved)) toast(fmt::format("{} was exported", label));
			if (!result.error.empty()) {
				fmt::print("WARN [Export] {} Recovery copy: {}\n", result.error, transfer->file().string());
				s_message = fmt::format("Export failed: {}\n\nThe Norn was kept. Recovery copy: {}", result.error, transfer->file().string());
			}
		});
	} catch (const std::exception& e) {
		s_native_drag_active = false;
		s_message = fmt::format("Couldn't export the creature: {}", e.what());
	}
}

// the Testing menu's items: the classic menu bar's Testing menu, and the modern Settings window
void draw_testing_items() {
		if (ImGui::MenuItem("Create a male norn")) {
		create_test_norn(1);
	}
	if (ImGui::MenuItem("Create a female norn")) {
		create_test_norn(2);
	}
	auto* c = selected_creature();
	if (ImGui::MenuItem("Force Ageing", nullptr, false, c && !c->dead && c->age < 7)) {
		c->loci.life_stage[std::min<size_t>(c->age, 6)] = 1;
		c->age_update();
	}
	if (ImGui::MenuItem("Euthanasia", nullptr, false, c && !c->dead)) {
		c->die();
	}
	ImGui::Separator();
	ImGui::MenuItem("Show script errors", nullptr, &s_options.show_script_errors);
	ImGui::MenuItem("Debug console", "F12", &s_show_debug_console);
	ImGui::MenuItem("Show rooms", nullptr, &g_engine_context.map->show_rooms);
	ImGui::MenuItem("Show ground level", nullptr, &g_engine_context.map->show_ground_level);
}

// The modern layout's world plaque (not in the original): the File menu's work as brass
// buttons at the top left, the rest of the menus being on the bar or in Settings. Where the
// window is too narrow for it and the clock, it folds into one button with a menu.
float bar_scale();

float s_clock_width = 0; // the clock's capsule, as wide as it was last drawn in full

// debugging: opens the popup if it's the one asked for
void open_test_popup(const char* id) {
	if (s_test_popup == id) {
		ImGui::OpenPopup(id);
		s_test_popup.clear();
	}
}

// the room the clock's score takes (the first thing to go in a narrow window)
float clock_score_width() {
	return ImGui::GetTextLineHeight() + 4 + ImGui::CalcTextSize("00000").x + ImGui::GetStyle().ItemSpacing.x;
}

// a menu item's label, moved right for a picture before it
std::string indented(const char* label) {
	return fmt::format("          {}", label);
}

// where a menu item's picture goes, its item starting at p
void item_picture(UIArt::Picture picture, ImVec2 p, bool lit = false) {
	const float h = ImGui::GetTextLineHeight();
	UIArt::picture(ImGui::GetWindowDrawList(), picture, ImVec2(p.x + h * 0.75f, p.y + h / 2), h * 1.3f, lit);
}

// A menu item with one of the bar's pictures before its label
bool picture_item(UIArt::Picture picture, const char* label, const char* shortcut = nullptr, bool selected = false, bool enabled = true) {
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool clicked = ImGui::MenuItem(indented(label).c_str(), shortcut, selected, enabled);
	const int first = ImGui::GetWindowDrawList()->VtxBuffer.Size;
	item_picture(picture, p);
	if (!enabled) {
		auto& vtx = ImGui::GetWindowDrawList()->VtxBuffer;
		for (int i = first; i < vtx.Size; ++i) {
			vtx[i].col = (vtx[i].col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(((vtx[i].col >> IM_COL32_A_SHIFT) & 0xFF) * 0.35f) << IM_COL32_A_SHIFT);
		}
	}
	return clicked;
}

void help_items() {
	if (ImGui::MenuItem("Help", "F1")) {
		s_show_help = true;
	}
	if (ImGui::MenuItem("Tip of the day")) {
		open_tip();
	}
	if (ImGui::MenuItem("Crash reports and logs")) {
		s_show_crash_window = true;
	}
	ImGui::Separator();
	if (ImGui::MenuItem("About")) {
		s_show_about = true;
	}
}

// returns where the plaque ends on the right
float draw_world_plaque() {
	const float k = bar_scale();
	const float r = 16 * k, step = 2 * r + 7 * k, pad = 9 * k;
	using UIArt::Picture;
	auto* c = selected_creature();
	enum Action { New, Load, Save, SaveAs, Import, Export, Record, Settings, Help, Quit };
	struct Button {
		const char* id;
		Picture picture;
		const char* label;
		const char* shortcut;
		bool enabled;
	};
#ifdef __APPLE__
	static const char* keys[] = {"Cmd+N", "Cmd+O", "Cmd+S", "Cmd+Shift+S", "Cmd+,"};
#else
	static const char* keys[] = {"Ctrl+N", "Ctrl+O", "Ctrl+S", "Ctrl+Shift+S", "Ctrl+,"};
#endif
	const Button buttons[] = {
		{"##newworld", Picture::NewWorld, "New world", keys[0], true},
		{"##loadworld", Picture::Open, "Load a world", keys[1], true},
		{"##saveworld", Picture::Save, "Save", keys[2], true},
		{"##saveas", Picture::SaveAs, "Save as", keys[3], true},
		{"##import", Picture::Import, "Import an egg or creature", nullptr, can_import()},
		{"##export", Picture::Export, EggFiles::held_egg() ? "Export the egg in the hand" : "Export the chosen creature", nullptr, EggFiles::held_egg() || can_export(c)},
		{"##record", Picture::Record, Recording::recording() ? "Recording" : "Record or play back", nullptr, true},
		{"##settings", Picture::Settings, "Settings", keys[4], true},
		{"##help", Picture::Help, "Help", "F1", true},
		{"##quit", Picture::Quit, "Save and quit", nullptr, true},
	};
	auto act = [&](int action) {
		switch (action) {
			case New: s_show_new = true; break;
			case Load: s_show_open = true; break;
			case Save: save_world_now(); break;
			case SaveAs: s_show_save_as = true; break;
			case Import: choose_import_files(); break;
			case Export:
				if (EggFiles::held_egg()) {
					choose_export_egg();
				} else if (c) {
					choose_export_file(*c);
				}
				break;
			case Record: ImGui::OpenPopup("##recordmenu"); break;
			case Settings: s_show_preferences = true; break;
			case Help: ImGui::OpenPopup("##helpmenu"); break;
			case Quit:
				if (save_world_now()) {
					s_quit = true;
				}
				break;
		}
	};
	const int count = static_cast<int>(std::size(buttons));
	const float full = pad * 2 + step * static_cast<float>(count) - 7 * k;
	const float W = ImGui::GetIO().DisplaySize.x;
	// (the clock gives up its score first)
	const bool folded = 12 + full + 12 * k + s_clock_width - clock_score_width() + 12 > W;
	const ImVec2 size(folded ? pad * 2 + 2 * r : full, pad * 2 + 2 * r);
	const ImVec2 pos(12, 12);
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	if (ImGui::Begin("##worldplaque", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
		auto* draw = ImGui::GetWindowDrawList();
		UIArt::wood_panel(draw, pos, ImVec2(pos.x + size.x, pos.y + size.y), size.y / 2, 0.15f);
		draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), UIArt::BRASS, size.y / 2, 0, 2);
		if (folded) {
			if (UIArt::round_button(draw, "##worldmenubutton", ImVec2(pos.x + size.x / 2, pos.y + size.y / 2), r, Picture::Menu, "World", Recording::recording())) {
				ImGui::OpenPopup("##worldmenu");
			}
			open_test_popup("##worldmenu");
			if (ImGui::BeginPopup("##worldmenu")) {
				for (int i = 0; i < count; ++i) {
					const auto& b = buttons[i];
					if (i == Record || i == Help) {
						const ImVec2 p = ImGui::GetCursorScreenPos();
						const bool open = ImGui::BeginMenu(indented(b.label).c_str());
						item_picture(b.picture, p, i == Record && Recording::recording());
						if (open) {
							if (i == Record) {
								RecordingUI::menu_items();
							} else {
								help_items();
							}
							ImGui::EndMenu();
						}
					} else if (picture_item(b.picture, b.label, b.shortcut, false, b.enabled)) {
						act(i);
					}
					if (i == SaveAs || i == Export || i == Help) {
						ImGui::Separator();
					}
				}
				ImGui::EndPopup();
			}
		} else {
			for (int i = 0; i < count; ++i) {
				const auto& b = buttons[i];
				const ImVec2 at(pos.x + pad + r + step * static_cast<float>(i), pos.y + size.y / 2);
				const std::string tip = b.shortcut ? fmt::format("{} ({})", b.label, b.shortcut) : std::string(b.label);
				if (UIArt::round_button(draw, b.id, at, r, b.picture, tip.c_str(), i == Record && Recording::recording(), b.enabled)) {
					act(i);
				}
			}
		}
		if (ImGui::BeginPopup("##recordmenu")) {
			RecordingUI::menu_items();
			ImGui::EndPopup();
		}
		if (ImGui::BeginPopup("##helpmenu")) {
			help_items();
			ImGui::EndPopup();
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
	return pos.x + size.x;
}

void draw_menu_bar() {
	if (!ImGui::BeginMainMenuBar()) {
		return;
	}
	if (ImGui::BeginMenu("File")) {
		if (ImGui::MenuItem("New world...", "Ctrl+N")) s_show_new = true;
		if (ImGui::MenuItem("Load world...", "Ctrl+O / F9")) s_show_open = true;
		if (ImGui::MenuItem("Save game", "Ctrl+S / F5")) save_world_now();
		if (ImGui::MenuItem("Save game as...", "Ctrl+Shift+S")) s_show_save_as = true;
		ImGui::Separator();
		ImGui::MenuItem("Web Connect", nullptr, false, false);
		if (ImGui::MenuItem("Export current creature", nullptr, false, can_export(selected_creature()))) {
			choose_export_file(*selected_creature());
		}
		ImGui::SetItemTooltip("%s", GameText::line(GameText::Original{26, 0xa5734a77661179d3ull}, "Export the chosen creature to a file").c_str());
		if (ImGui::MenuItem("Import creature", nullptr, false, can_import())) {
			choose_import_files();
		}
		// LibreCreatures' egg files (not in the original)
		if (ImGui::MenuItem("Export Held Egg...", nullptr, false, EggFiles::held_egg() != nullptr)) {
			choose_export_egg();
		}
		ImGui::SetItemTooltip("Pick up an egg with the hand to export it to a .egg file; it leaves the world");
		if (ImGui::MenuItem("Import Egg...", nullptr, false, can_import())) {
			choose_import_egg();
		}
		ImGui::SetItemTooltip("%s", GameText::line(GameText::Original{39, 0x717ba15bee037542ull}, "Import a creature from a file").c_str());
		ImGui::Separator();
		if (ImGui::MenuItem(session_profile().empty() ? "Save and quit" : "Save and return to game/world selection") && save_world_now()) {
			s_quit = true;
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("World")) {
		if (ImGui::MenuItem("New world...", "Ctrl+N")) {
			s_show_new = true;
		}
		if (ImGui::MenuItem("Load world...", "Ctrl+O")) {
			s_show_open = true;
		}
		if (ImGui::MenuItem("Save game", "Ctrl+S / F5")) {
			save_world_now();
		}
		if (ImGui::MenuItem("Save game as...")) {
			s_show_save_as = true;
		}
		ImGui::Separator();
		ImGui::MenuItem("Infinite scroll", nullptr, &g_engine_context.viewport->infinite_scroll);
		if (ImGui::MenuItem("Instant verb vocabulary", nullptr, false, selected_creature() != nullptr)) {
			instant_verb_vocabulary();
		}
		ImGui::Separator();
		if (ImGui::MenuItem(Wolfling::active() ? "Wolfling run..." : "Start a wolfling run...")) {
			if (Wolfling::active()) {
				Wolfling::open_dashboard();
			} else {
				Wolfling::open_start_sheet();
			}
		}
		ImGui::Separator();
		RecordingUI::menu_items();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Testing")) {
		draw_testing_items();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("View")) {
		ImGui::MenuItem("Toolbar", nullptr, &s_options.show_toolbar);
		ImGui::MenuItem("Status Bar", nullptr, &s_options.show_status_bar);
		// not in the original: switching between the two interfaces, and the preferences
		ImGui::Separator();
		ImGui::MenuItem("Classic Interface", nullptr, &s_options.classic_layout);
		ImGui::SetItemTooltip("The original's look; untick for the modern interface");
		if (ImGui::MenuItem("Family Tree...")) {
			Family::open_tree();
		}
		if (ImGui::MenuItem("Preferences...", "Ctrl+,")) {
			s_show_preferences = true;
		}
		ImGui::EndMenu();
	}
	// Camera is a menu of its own, as in the original (batch 6, Q10)
	if (ImGui::BeginMenu("Camera")) {
		{
			ImGui::MenuItem("Track Norn", nullptr, &s_options.track_norn);
			ImGui::MenuItem("Smooth Scrolling", nullptr, &g_engine_context.viewport->smooth_scrolling);
			if (ImGui::MenuItem("Which is my norn?", nullptr, false, selected_creature() != nullptr)) {
				select_creature(*selected_creature());
				s_which_is_my_norn_until = ImGui::GetTime() + 3;
			}
			ImGui::Separator();
			if (ImGui::MenuItem(GameText::line(GameText::Original{26, 0x21bd9c27ffe01939ull}, "Add This View to Favourites...").c_str())) {
				s_show_add_place = true;
			}
			if (ImGui::MenuItem(GameText::line(GameText::Original{31, 0xd615a199a562ebb1ull}, "Remove a Favourite...").c_str())) {
				s_show_remove_place = true;
			}
			ImGui::Separator();
			for (auto& place : g_engine_context.world->favorite_places) {
				if (!place.name.empty() && ImGui::MenuItem(place.name.c_str())) {
					auto r = to_rectf(g_engine_context.viewport->get_main_camera_rect());
					g_engine_context.viewport->centre_on(place.x + r.width / 2, place.y + r.height / 2);
				}
			}
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Tools")) {
		ImGui::MenuItem("Creature's View", nullptr, &s_options.creatures_view);
		ImGui::Separator();
		Kits::draw_menu_items();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Norns")) {
		bool any = false;
		for (auto* obj : *g_engine_context.objects) {
			if (auto* c = obj->as_creature()) {
				any = true;
				ImGui::PushID(c);
				if (ImGui::MenuItem(creature_label(*c).c_str(), nullptr, c->uid == g_engine_context.world->current_norn)) {
					select_creature(*c);
				}
				ImGui::PopID();
			}
		}
		if (!any) {
			ImGui::MenuItem("None available", nullptr, false, false);
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Help")) {
		if (ImGui::MenuItem("Help Topics", "F1")) {
			s_show_help = true;
		}
		if (ImGui::MenuItem("Tip of the Day")) {
			open_tip();
		}
		if (ImGui::MenuItem("Crash reports and logs...")) {
			s_show_crash_window = true;
		}
		ImGui::Separator();
		if (ImGui::MenuItem("About Creatures...")) {
			s_show_about = true;
		}
		ImGui::EndMenu();
	}
	ImGui::EndMainMenuBar();
}

// --- the toolbar (Q1) ---

// starts a new toolbar row when the next item of this width wouldn't fit

float draw_toolbar(float top) {
	if (!s_options.show_toolbar) {
		return 0;
	}
	ImGui::SetNextWindowPos(ImVec2(0, top));
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	float height = 0;
	if (ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
		// The original's toolbar (batch 6, Q10): the speech box, then buttons with the 16 x 16
		// pictures of Creatures.exe's bitmap 128, with 10 px gaps between the groups.
		const UIImages::Bitmap* strip = UIImages::exe_bitmap("Creatures.exe", 128);
		auto gap = [] {
			ImGui::SameLine(0, 10);
		};
		auto picture_button = [strip](const char* id, int picture, const char* tooltip, bool checked = false, bool enabled = true) {
			ImGui::SameLine(0, 1);
			ImGui::BeginDisabled(!enabled);
			if (checked) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			}
			bool pressed = false;
			const float size = 16 * UIStyle::scale();
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));
			if (strip) {
				const float w = static_cast<float>(strip->width);
				pressed = ImGui::ImageButton(id, strip->texture, ImVec2(size, size), ImVec2(16.f * static_cast<float>(picture) / w, 0), ImVec2(16.f * static_cast<float>(picture + 1) / w, 1));
			} else {
				pressed = ImGui::Button(id, ImVec2(size + 6, size + 6));
			}
			ImGui::PopStyleVar();
			if (checked) {
				ImGui::PopStyleColor();
			}
			ImGui::EndDisabled();
			ImGui::SetItemTooltip("%s", tooltip);
			return pressed;
		};

		// typing to creatures (Q7): the speech box, 150 px
		ImGui::SetNextItemWidth(150 * UIStyle::scale());
		bool said = ImGui::InputText("##say", s_sentence, sizeof(s_sentence), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SetItemTooltip("%s", GameText::line(GameText::Original{52, 0xabd92aec6f77c822ull}, "Type something, then click SAY").c_str());
		gap();
		if (picture_button("##speak", 0, "Speak text") || said) {
			say(s_sentence);
			s_sentence[0] = '\0';
		}
		gap();
		if (picture_button("##help", 1, "Help")) {
			s_show_help = true;
		}
		gap();
		picture_button("##web", 19, "Web Connect", false, false);
		gap();
		if (picture_button("##play", 20, "Play", !s_paused)) {
			s_paused = false;
		}
		if (picture_button("##pause", 21, "Pause", s_paused)) {
			s_paused = true;
		}
		gap();
		if (picture_button("##track", 3, "Track Norn", s_options.track_norn)) {
			s_options.track_norn = !s_options.track_norn;
		}
		if (picture_button("##which", 5, "Which is my Norn", false, selected_creature() != nullptr)) {
			s_which_is_my_norn_until = ImGui::GetTime() + 3;
		}
		if (picture_button("##view", 4, "Creature's view", s_options.creatures_view)) {
			s_options.creatures_view = !s_options.creatures_view;
		}
		gap();
		// the kits, in the shipped registry's order; Tool1 has no kit, so it's a gap. Which
		// pictures the original showed is unknown: these are the strip's kit-like pictures.
		struct KitButton {
			const char* kit;
			int picture;
			const char* tooltip;
		};
		static const KitButton KIT_BUTTONS[] = {
			{"Hatchery", 6, "Hatch a new norn"}, {nullptr, 0, nullptr}, {"Owner", 8, "Owner's Kit"},
			{"Health", 9, "Health Kit"}, {"Science", 10, "Science Kit"}, {"Breeder", 11, "Breeder's Kit"},
			{"Observe", 12, "Observation Kit"}, {"Injector", 13, "Object Injector"}, {"Score", 14, "Performance Kit"},
			{"Funeral", 15, "Creature Graveyard"},
		};
		for (const auto& k : KIT_BUTTONS) {
			if (!k.kit) {
				gap();
				continue;
			}
			if (picture_button(fmt::format("##kit{}", k.kit).c_str(), k.picture, k.tooltip, Kits::is_open(k.kit), Wolfling::kit_allowed(k.kit))) {
				Kits::toggle(k.kit);
			}
		}
		gap();
		plushie_toggle(ImGui::GetFrameHeight());
		// Ctrl+S speaks (the original's accelerator)
		if (ImGui::IsKeyPressed(ImGuiKey_S, false) && ImGui::GetIO().KeyCtrl) {
			say(s_sentence);
			s_sentence[0] = '\0';
		}
		height = ImGui::GetWindowHeight();
	}
	ImGui::End();
	ImGui::PopStyleVar();
	return height;
}

// --- the status bar (Q1, Q31) ---

float draw_status_bar() {
	if (!s_options.show_status_bar) {
		return 0;
	}
	ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetIO().DisplaySize.y), 0, ImVec2(0.0f, 1.0f));
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	float height = 0;
	if (ImGui::Begin("StatusBar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
		// The original's status bar (batch 6, Q10): help text, four event panes ("BIRTH" for a
		// living creature, "DEATH" for a dead one, " EGG " for anything else; newest on the
		// left, double-click to go there), a spacer, "Life Force nnn%" (glycogen, refreshed
		// every 100 ticks; 0 when dead, greyed with no creature) and "Score" (capped at 99999).
		auto* c = selected_creature();
		const uint32_t tick = g_engine_context.world->tick;
		if (c && (s_health < 0 || tick - s_health_tick >= HEALTH_REFRESH_TICKS || tick < s_health_tick)) {
			s_health = creature_health_percent(*c);
			s_health_tick = tick;
		}
		if (!c) {
			s_health = -1;
		}
		auto pane = [](const char* id, const std::string& text, const char* width_template, bool enabled) {
			const ImVec2 pad = ImGui::GetStyle().FramePadding;
			const float w = ImGui::CalcTextSize(width_template).x + pad.x * 2;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			const ImVec2 size(w, ImGui::GetTextLineHeight() + pad.y * 2);
			ImGui::InvisibleButton(id, size);
			auto* draw = ImGui::GetWindowDrawList();
			// sunken, like a Windows status bar pane
			draw->AddLine(at, ImVec2(at.x + size.x, at.y), ImGui::GetColorU32(ImGuiCol_Border));
			draw->AddLine(at, ImVec2(at.x, at.y + size.y), ImGui::GetColorU32(ImGuiCol_Border));
			draw->AddLine(ImVec2(at.x, at.y + size.y), ImVec2(at.x + size.x, at.y + size.y), ImGui::GetColorU32(ImGuiCol_BorderShadow));
			draw->AddLine(ImVec2(at.x + size.x, at.y), ImVec2(at.x + size.x, at.y + size.y), ImGui::GetColorU32(ImGuiCol_BorderShadow));
			draw->AddText(ImVec2(at.x + pad.x, at.y + pad.y), ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), text.c_str());
		};
		const ImVec2 pad = ImGui::GetStyle().FramePadding;
		auto width_of = [&](const char* t) {
			return ImGui::CalcTextSize(t).x + pad.x * 2 + ImGui::GetStyle().ItemSpacing.x;
		};
		const float right = width_of("DEATH") * 4 + width_of("xx") + width_of("Life Force 100%") + width_of("Score xxxxxx");
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(s_paused ? "Paused" : "For Help, press F1");
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - right - ImGui::GetStyle().WindowPadding.x));

		std::vector<Object*> events;
		for (auto handle : g_engine_context.world->events) {
			if (auto* obj = g_engine_context.objects->try_get(handle)) {
				events.push_back(obj);
			}
		}
		for (int i = 0; i < 4; ++i) {
			// pane 1 is the newest
			Object* obj = i < static_cast<int>(events.size()) ? events[events.size() - 1 - static_cast<size_t>(i)] : nullptr;
			std::string text;
			if (obj) {
				auto* ec = obj->as_creature();
				text = ec ? (ec->dead ? "DEATH" : "BIRTH") : " EGG ";
			}
			pane(fmt::format("##event{}", i).c_str(), text, "DEATH", obj != nullptr);
			if (obj && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				auto b = obj->get_bbox();
				g_engine_context.viewport->centre_on(b.x + b.width / 2, b.y + b.height / 2);
			}
			if (obj) {
				ImGui::SetItemTooltip("%s", obj->as_creature() ? creature_label(*obj->as_creature()).c_str() : get_object_name(obj).c_str());
			}
			ImGui::SameLine();
		}
		ImGui::Dummy(ImVec2(ImGui::CalcTextSize("xx").x, 1));
		ImGui::SameLine();
		pane("##life", fmt::format("Life Force {:3}%", std::max(0, s_health)), "Life Force 100%", c != nullptr);
		if (c) {
			ImGui::SetItemTooltip("%s", creature_label(*c).c_str());
		}
		ImGui::SameLine();
		pane("##score", fmt::format("Score {:>6}", player_score()), "Score xxxxxx", true);

		height = ImGui::GetWindowHeight();
	}
	ImGui::End();
	ImGui::PopStyleVar();
	return height;
}

// --- the modern layout ---
//
// A modern take on the same functions: one panel along the bottom with the selected Norn,
// every creature's face and the tools (speech is typed straight to the hand: Enter); news as notifications that can be
// clicked to look; a small clock with pause and speed; and the Creature's View porthole.
// The classic layout (the original's toolbar and status bar) is a preference.

int32_t s_speed = 1;

struct Toast {
	std::string text;
	ObjectHandle object;
	double until = 0;
	bool error = false;
	std::string details;
};
std::vector<Toast> s_toasts;
std::vector<ObjectHandle> s_known_events;
std::vector<std::string> s_announced_deaths;
bool s_news_primed = false;
std::string s_error_details;
bool s_show_error_details = false;

constexpr double TOAST_SECONDS = 7;
constexpr size_t MAX_TOASTS = 5;
constexpr float HUD_PAD = 8;

std::string short_name(const Creature& c) {
	return c.history_name.empty() || c.history_name == "<Un Named>" ? c.moniker : c.history_name;
}

const char* stage_name(int32_t age) {
	static const char* names[] = {"Baby", "Child", "Adolescent", "Youth", "Adult", "Old", "Senile", "Dead"};
	return names[std::clamp(age, 0, 7)];
}

std::string object_label(Object* obj) {
	if (!obj) {
		return "";
	}
	if (auto* c = obj->as_creature()) {
		return short_name(*c);
	}
	std::string name = get_object_name(obj);
	return name.empty() || name == "unknown" ? fmt::format("object {} {} {}", obj->family, obj->genus, obj->species) : name;
}

void toast(std::string text, ObjectHandle object, bool error, std::string details) {
	s_toasts.push_back({std::move(text), object, ImGui::GetTime() + TOAST_SECONDS, error, std::move(details)});
	if (s_toasts.size() > MAX_TOASTS) {
		s_toasts.erase(s_toasts.begin());
	}
}

void look_at(Object* obj) {
	if (obj) {
		auto b = obj->get_bbox();
		g_engine_context.viewport->centre_on(b.x + b.width / 2, b.y + b.height / 2);
	}
}

void watch_for_news() {
	// new events (births, eggs) and deaths become notifications; what's already there when a
	// world loads doesn't
	auto& events = g_engine_context.world->events;
	for (auto handle : events) {
		if (!s_news_primed || std::find(s_known_events.begin(), s_known_events.end(), handle) != s_known_events.end()) {
			continue;
		}
		auto* obj = g_engine_context.objects->try_get(handle);
		if (!obj) {
			continue;
		}
		if (auto* c = obj->as_creature()) {
			toast(fmt::format("{} has hatched!", short_name(*c)), handle);
		} else if (obj->family == 2 && obj->genus == 5) {
			toast("A new egg", handle);
		} else {
			toast(object_label(obj), handle);
		}
	}
	s_known_events = events;
	for (auto* obj : *g_engine_context.objects) {
		auto* c = obj->as_creature();
		if (!c || !c->dead || std::find(s_announced_deaths.begin(), s_announced_deaths.end(), c->moniker) != s_announced_deaths.end()) {
			continue;
		}
		s_announced_deaths.push_back(c->moniker);
		if (s_news_primed) {
			toast(fmt::format("{} has died", short_name(*c)), c->uid);
		}
	}
	s_news_primed = true;
}

// Window flags for the floating, undecorated parts of the modern layout.
constexpr ImGuiWindowFlags FLOATING = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

void draw_toasts(float top) {
	// notifications: capsules under the menu bar that fade out; click to look
	const double now = ImGui::GetTime();
	s_toasts.erase(std::remove_if(s_toasts.begin(), s_toasts.end(), [&](const Toast& t) { return t.until < now; }), s_toasts.end());
	float y = top + HUD_PAD * 1.5f;
	for (size_t i = 0; i < s_toasts.size(); ++i) {
		auto& t = s_toasts[i];
		const float fade = static_cast<float>(std::clamp((t.until - now) / 0.6, 0.0, 1.0));
		const float appear = static_cast<float>(std::clamp((now - (t.until - TOAST_SECONDS)) / 0.25, 0.0, 1.0));
		ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2, y - (1 - appear) * 10), 0, ImVec2(0.5f, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.05f, std::min(fade, appear)));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 40);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 8));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.17f, 0.17f, 0.18f, 0.96f));
		const std::string id = fmt::format("##toast{}", i);
		if (ImGui::Begin(id.c_str(), nullptr, FLOATING)) {
			auto* obj = g_engine_context.objects->try_get(t.object);
			const float face = ImGui::GetTextLineHeight() * 1.5f;
			if (auto* c = obj ? obj->as_creature() : nullptr) {
				ImVec2 p = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(face, face));
				UIImages::draw_creature_face(ImGui::GetWindowDrawList(), *c, p, ImVec2(p.x + face, p.y + face));
				ImGui::SameLine();
			} else if (t.error) {
				ImVec2 p = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(face * 0.8f, face));
				ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + face * 0.4f, p.y + face * 0.5f), face * 0.36f, IM_COL32(255, 69, 58, 255));
				ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + face * 0.4f - ImGui::CalcTextSize("!").x / 2, p.y + (face - ImGui::GetTextLineHeight()) / 2), IM_COL32_WHITE, "!");
				ImGui::SameLine();
			}
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (face - ImGui::GetTextLineHeight()) / 2);
			ImGui::TextUnformatted(t.text.c_str());
			if (obj || t.error) {
				// a chevron: there's more to see (where it is, or the error's details)
				ImGui::SameLine();
				const float h = ImGui::GetTextLineHeight();
				const ImVec2 p = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(h * 0.7f, h));
				UIArt::glyph(ImGui::GetWindowDrawList(), UIArt::Glyph::Chevron, ImVec2(p.x + h * 0.35f, p.y + h / 2), h * 0.8f, ImGui::GetColorU32(ImGuiCol_TextDisabled));
			}
			if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				if (t.error) {
					s_error_details = t.details;
					s_show_error_details = true;
				} else {
					look_at(obj);
				}
				t.until = now;
			}
			UIStyle::window_shadow();
			y += ImGui::GetWindowHeight() + 8;
		}
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
	}
}

void draw_clock(float top, float left) {
	// The game's clock, pause and speed, and the score, in a capsule at the top right. When the
	// window is too narrow for it beside the plaque (ending at left), the score goes.
	const float W = ImGui::GetIO().DisplaySize.x;
	const float star = ImGui::GetTextLineHeight(), score_w = clock_score_width();
	const bool compact = s_clock_width > W - HUD_PAD * 1.5f - left - 12;
	ImGui::SetNextWindowPos(ImVec2(W - HUD_PAD * 1.5f, top + HUD_PAD * 1.5f), 0, ImVec2(1, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 40);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 7));
	if (ImGui::Begin("##clock", nullptr, FLOATING | ImGuiWindowFlags_NoBackground)) {
		// a little wooden plaque with a brass edge, like the bar
		const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
		auto* plaque = ImGui::GetWindowDrawList();
		plaque->PushClipRectFullScreen();
		UIArt::wood_panel(plaque, pos, ImVec2(pos.x + size.x, pos.y + size.y), size.y / 2, 0.15f);
		if (Wolfling::active()) {
			// a wolfling run: a darker amber that pulses gently
			const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.2f);
			plaque->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(92, 44, 2, static_cast<int>(165 + 45 * pulse)), size.y / 2);
			plaque->AddRect(ImVec2(pos.x + 3, pos.y + 3), ImVec2(pos.x + size.x - 3, pos.y + size.y - 3), IM_COL32(255, 168, 40, static_cast<int>(70 + 110 * pulse)),
				size.y / 2 - 3, 0, 1.5f);
		}
		plaque->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), UIArt::BRASS, size.y / 2, 0, 2);
		plaque->PopClipRect();
		if (UIStyle::icon_button("##pause", s_paused ? UIStyle::Icon::Play : UIStyle::Icon::Pause, nullptr, s_paused ? "Resume (Space)" : "Pause (Space)", s_paused)) {
			s_paused = !s_paused;
		}
		// The day: a dial with the sun or the moon on its way round, and the day's number (a
		// wolfling run's own days during a run)
		{
			const uint32_t tick = g_engine_context.world->tick;
			const float h = ImGui::GetFrameHeight();
			const ImVec2 at = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(h, h));
			DayNight::draw_dial(ImGui::GetWindowDrawList(), ImVec2(at.x + h / 2, at.y + h / 2), h * 0.42f, DayNight::time_of_day(tick));
			uint32_t day = DayNight::day_number(tick);
			if (Wolfling::active()) {
				day = day + 1 - std::min(day, DayNight::day_number(Wolfling::run_started_tick()));
			}
			// the world's running time, to the second, for whoever wants it
			const uint32_t seconds = tick / 10;
			const std::string about = fmt::format("{} {} \xc2\xb7 {}:{:02}:{:02} in this world", Wolfling::active() ? "Day of the wolfling run:" : "Day", day,
				seconds / 3600, seconds / 60 % 60, seconds % 60);
			ImGui::SetItemTooltip("%s", about.c_str());
			ImGui::SameLine(0, 4);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetTextLineHeight()) / 2);
			// steady, so the capsule (held by its right edge) doesn't shift as the days go by
			UIText::steady_text(fmt::format("Day {}", day).c_str(), "Day 000", ImGui::GetColorU32(ImGuiCol_Text), 0);
			ImGui::SetItemTooltip("%s", about.c_str());
			ImGui::SameLine();
		}
		// Max runs as many ticks as the computer can while still drawing (for wolfling runs)
		static const char* speeds[] = {"1\xc3\x97", "2\xc3\x97", "4\xc3\x97", "Max"};
		int selected = s_speed >= 99 ? 3 : s_speed >= 4 ? 2 : s_speed >= 2 ? 1 : 0;
		if (UIStyle::segmented("##speed", speeds, 4, &selected)) {
			set_game_speed(selected == 3 ? 99 : selected == 2 ? 4 : selected == 1 ? 2 : 1);
			// F033: speed selection must preserve the player's pause state.
		}
		ImGui::SetItemTooltip("Game speed");
		if (Wolfling::active()) {
			// the run's paw badge: its records
			ImGui::SameLine();
			const float r = ImGui::GetFrameHeight() * 0.5f;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			if (UIArt::round_button(ImGui::GetWindowDrawList(), "##wolfling_badge", ImVec2(at.x + r, at.y + r), r, UIArt::Picture::Paw, "This wolfling run", true)) {
				Wolfling::open_dashboard();
			}
		}
		if (!compact) {
			// the score: a star and the number
			ImGui::SameLine();
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float fh = ImGui::GetFrameHeight();
			ImGui::Dummy(ImVec2(star, fh));
			UIArt::glyph(ImGui::GetWindowDrawList(), UIArt::Glyph::Star, ImVec2(p.x + star / 2, p.y + fh / 2), star * 0.95f, UIArt::BRASS_LIGHT);
			ImGui::SetItemTooltip("Score");
			ImGui::SameLine(0, 4);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (fh - ImGui::GetTextLineHeight()) / 2);
			UIText::steady_text(std::to_string(player_score()).c_str(), "00000", ImGui::GetColorU32(ImGuiCol_Text), 1);
			ImGui::SetItemTooltip("Score: 256 for each egg laid, plus your creatures' time");
		}
		s_clock_width = ImGui::GetWindowWidth() + (compact ? score_w : 0);
		// (the wooden plaque casts the interface's shadow itself)
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
}

// a round avatar with the creature's face centred in it
// The badge's outline as the draw list's path, grown by `out` pixels: the circle round the face
// and the rounded tab to the bottom left, joined by their outer tangents.
void badge_outline(ImDrawList* draw, ImVec2 centre, float radius, float out) {
	constexpr float PI = 3.14159265f;
	const float r = radius * 1.1f + out;
	const float tab = radius * 0.46f + out;
	const float reach = radius * 1.12f;
	const float towards = PI * 0.75f; // bottom left, with y down
	const ImVec2 corner(centre.x + std::cos(towards) * reach, centre.y + std::sin(towards) * reach);
	const float side = std::acos(std::clamp((r - tab) / reach, -1.f, 1.f));
	draw->PathClear();
	draw->PathArcTo(centre, r, towards + side, towards + 2 * PI - side, 48);
	draw->PathArcTo(corner, tab, towards - side, towards + side, 12);
}

// The generation badge behind a face: a grey backing shaped like a circle with a rounded tab
// out to the bottom left (like the Half-Life logo, flipped), the generation number in white
// in the tab. The backing goes behind the face and the number on top (number_only).
void draw_generation_badge(ImDrawList* draw, ImVec2 centre, float radius, int generation, bool number_only) {
	constexpr float PI = 3.14159265f;
	const float tab = radius * 0.46f; // the tab's corner radius
	const float reach = radius * 1.12f; // from the centre to the tab's corner centre
	const float towards = PI * 0.75f; // bottom left, with y down
	const ImVec2 corner(centre.x + std::cos(towards) * reach, centre.y + std::sin(towards) * reach);
	if (!number_only) {
		badge_outline(draw, centre, radius, 0);
		draw->PathFillConvex(IM_COL32(74, 73, 75, 255));
		return;
	}
	const std::string text = std::to_string(generation);
	ImFont* font = ImGui::GetFont();
	const float size = tab * (text.size() > 1 ? 1.45f : 1.85f);
	const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
	const ImVec2 at(corner.x - ts.x / 2 - tab * 0.08f, corner.y - ts.y / 2 + tab * 0.08f);
	// a dark edge so it reads over the face, then the number (twice, for weight)
	for (float dx : {-1.f, 1.f}) {
		for (float dy : {-1.f, 1.f}) {
			draw->AddText(font, size, ImVec2(at.x + dx, at.y + dy), IM_COL32(40, 40, 44, 255), text.c_str());
		}
	}
	draw->AddText(font, size, at, IM_COL32_WHITE, text.c_str());
	draw->AddText(font, size, ImVec2(at.x + 0.6f, at.y), IM_COL32_WHITE, text.c_str());
}

void draw_avatar(ImDrawList* draw, const Creature& c, ImVec2 centre, float radius, bool selected, int generation) {
	const bool badge = generation > 0 && !s_options.classic_layout;
	if (badge) {
		draw_generation_badge(draw, centre, radius, generation, false);
	}
	draw->AddCircleFilled(centre, radius, selected ? IM_COL32(110, 190, 90, 60) : IM_COL32(118, 118, 128, 50), 40);
	if (selected && !badge) {
		draw->AddCircle(centre, radius + 1.5f, UIStyle::accent(), 40, 2.5f);
	}
	const float inner = radius * 1.72f; // the face fills most of the circle (heads are wide)
	UIImages::draw_creature_face(draw, c, ImVec2(centre.x - inner / 2, centre.y - inner / 2), ImVec2(centre.x + inner / 2, centre.y + inner / 2), c.dead ? IM_COL32(150, 150, 150, 160) : IM_COL32_WHITE);
	if (badge) {
		// the selection goes round the whole badge, tab and all
		if (selected) {
			badge_outline(draw, centre, radius, 1.5f);
			draw->PathStroke(UIStyle::accent(), ImDrawFlags_Closed, 2.5f);
		}
		draw_generation_badge(draw, centre, radius, generation, true);
	}
}

void plushie_toggle(float size) {
	bool& on = g_engine_context.pointer->m_editing;
	const bool allowed = !Wolfling::active();
	ImGui::BeginDisabled(!allowed);
	if (UIStyle::plushie_button("##plushie", !allowed ? "Privileged mode: off during a wolfling run" : on ? "Privileged mode: on" : "Privileged mode", on && allowed, size)) {
		on = !on;
		show_message(on ? "Privileged mode on" : "Privileged mode off");
	}
	ImGui::EndDisabled();
}

// --- the modern bar ---
//
// Not in the original. Along the bottom, a carved wooden plank with brass fittings, in the
// spirit of Albia's hand-made look (UIArt): the selected Norn's portrait in a medallion on the
// left, with its life force round it; Creature's View as a porthole on the right; the other
// Norns as small medallions along the top edge; and on the plank its name, the four panes
// (Norn, Lab, Objects, Records), the three things players do most (Give, Heal, Teach, each
// opening a tray above the bar) and a few small studs. The art is drawn behind every window;
// only the parts that can be clicked catch the mouse, so the rest of the world stays playable.


// the hand tickles (a reward) or slaps (a punishment) the Norn, as a click on its head or body
void touch_norn(Creature& c, bool tickle) {
	auto pointer = g_engine_context.pointer->m_pointer_tool;
	g_engine_context.messages->hand_message(pointer, c.uid, tickle ? MESSAGE_ACTIVATE1 : MESSAGE_DEACTIVATE);
	g_engine_context.pointer->run_hand_script(&c, tickle ? SCRIPT_POINTER_ACTIVATE1 : SCRIPT_POINTER_DEACTIVATE);
}

float bar_scale() {
	const auto& io = ImGui::GetIO();
	return UIStyle::scale() * std::clamp(std::min(io.DisplaySize.x / 1400.f, io.DisplaySize.y / 900.f), 0.8f, 1.35f);
}

// an invisible window over part of the bar, to catch the mouse there
bool begin_hit_area(const char* name, ImVec2 min, ImVec2 max) {
	ImGui::SetNextWindowPos(min);
	ImGui::SetNextWindowSize(ImVec2(std::max(1.f, max.x - min.x), std::max(1.f, max.y - min.y)));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	const bool open = ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar(2);
	if (open) {
		ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
	}
	return open;
}

// text at a font size, cut short with an ellipsis to fit a width
std::string fit_text(std::string text, float size, float width) {
	ImFont* font = ImGui::GetFont();
	if (font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x <= width) {
		return text;
	}
	while (!text.empty() && font->CalcTextSizeA(size, FLT_MAX, 0, (text + "\xe2\x80\xa6").c_str()).x > width) {
		// a whole character at a time (a UTF-8 character's continuation bytes, then its first)
		while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
			text.pop_back();
		}
		if (!text.empty()) {
			text.pop_back();
		}
	}
	while (!text.empty() && text.back() == ' ') {
		text.pop_back();
	}
	return text.empty() ? text : text + "\xe2\x80\xa6";
}

float s_quick_x[3] = {}; // where the bar's quick slots are, for their keys (1, 2, 3)

void toggle_tray(Tray tray, float x) {
	s_tray = s_tray == tray ? Tray::None : tray;
	s_tray_x = x;
}

void places_items() {
	bool any = false;
	for (auto& place : g_engine_context.world->favorite_places) {
		if (!place.name.empty()) {
			any = true;
			if (ImGui::MenuItem(place.name.c_str())) {
				auto r = to_rectf(g_engine_context.viewport->get_main_camera_rect());
				g_engine_context.viewport->centre_on(place.x + r.width / 2, place.y + r.height / 2);
			}
		}
	}
	if (!any) {
		ImGui::MenuItem("No favourite places yet", nullptr, false, false);
	}
	ImGui::Separator();
	if (ImGui::MenuItem("Add this view...")) {
		s_show_add_place = true;
	}
	if (ImGui::MenuItem("Remove...", nullptr, false, any)) {
		s_show_remove_place = true;
	}
}

void draw_places_menu() {
	if (ImGui::BeginPopup("##placesmenu")) {
		places_items();
		ImGui::EndPopup();
	}
}

// the bar's More menu: what's used now and then; everything, when the bar has folded its studs
void draw_more_menu(bool everything) {
	if (ImGui::BeginPopup("##moremenu")) {
		using UIArt::Picture;
		auto* c = selected_creature();
		if (everything) {
			if (picture_item(Picture::Follow, "Follow the chosen Norn", "F", s_options.track_norn)) {
				s_options.track_norn = !s_options.track_norn;
			}
			if (picture_item(Picture::Egg, "Hatchery", "H", Kits::is_open("Hatchery"))) {
				Kits::toggle("Hatchery");
			}
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const bool places = ImGui::BeginMenu(indented("Favourite places").c_str());
			item_picture(Picture::Places, p);
			if (places) {
				places_items();
				ImGui::EndMenu();
			}
			if (picture_item(Picture::Tree, "Family tree")) {
				Family::open_tree();
			}
			ImGui::Separator();
		}
		if (picture_item(g_creature_speech_audible ? Picture::Voices : Picture::Quiet, "Voices", nullptr, g_creature_speech_audible)) {
			g_creature_speech_audible = !g_creature_speech_audible;
		}
		if (picture_item(Picture::Paw, Wolfling::active() ? "This wolfling run" : "Start a wolfling run...")) {
			if (Wolfling::active()) {
				Wolfling::open_dashboard();
			} else {
				Wolfling::open_start_sheet();
			}
		}
		bool& privileged = g_engine_context.pointer->m_editing;
		if (ImGui::MenuItem(indented("Privileged mode").c_str(), nullptr, privileged, !Wolfling::active())) {
			privileged = !privileged;
			show_message(privileged ? "Privileged mode on" : "Privileged mode off");
		}
		ImGui::SetItemTooltip("The original's hidden cheat: the hand picks up creatures and the Hatchery never runs out");
		ImGui::Separator();
		if (ImGui::MenuItem(indented("Teach every action word now").c_str(), nullptr, false, c != nullptr)) {
			instant_verb_vocabulary();
		}
		ImGui::EndPopup();
	}
}

// returns the height the world view should leave for it (the plank; the medallions rise
// over the bottom of the view)
float draw_modern_bar() {
	auto& io = ImGui::GetIO();
	const float W = io.DisplaySize.x, H = io.DisplaySize.y;
	const float k = bar_scale();
	const float m = 8 * k, R = 62 * k, P = 72 * k;
	const float py1 = H - m, py0 = py1 - P;
	// the portrait and the porthole in the window's corners, the plank right across
	const float x0 = m, x1 = W - m;
	const ImVec2 orb_l(x0 + R + 2 * k, H - m - R - 4 * k), orb_r(x1 - R - 2 * k, H - m - R - 4 * k);
	const float inner_l = orb_l.x + R + 12 * k, inner_r = orb_r.x - R - 12 * k;
	auto* draw = ImGui::GetBackgroundDrawList();
	auto* c = selected_creature();

	// the layout: quick slots in the middle; panes and the name to the left; studs to the right.
	// Narrow windows lose the name's second line, then fold the studs and panes into menus.
	const float Q = 31 * k, r = 19 * k, gap = 12 * k;
	const float quick_w = 3 * 2 * Q + 2 * gap;
	const float stud = 2 * r + 8 * k;
	const float mid = (inner_l + inner_r) / 2;
	const float quick_l = mid - quick_w / 2, quick_r = mid + quick_w / 2;
	const float left_room = quick_l - gap - inner_l, right_room = inner_r - (quick_r + gap);
	const float panes_w = 4 * stud;
	// the studs: follow, the Hatchery, places, the family tree and More, and privileged mode's
	// plushie while it's on
	const bool privileged = g_engine_context.pointer->m_editing && !Wolfling::active();
	const bool fold_studs = right_room < static_cast<float>(privileged ? 6 : 5) * stud;
	const bool fold_panes = left_room < panes_w + 40 * k;
	const float name_w = std::max(0.f, left_room - (fold_panes ? stud : panes_w) - gap);

	// the plank, right across the window and down to its bottom edge, under the medallions
	UIArt::wood_panel(draw, ImVec2(-4, py0), ImVec2(W + 4, H + 4), 0);
	draw->AddRectFilled(ImVec2(0, py0 - 2 * k), ImVec2(W, py0 + 3 * k), UIArt::BRASS_DARK);
	draw->AddLine(ImVec2(0, py0 - 1 * k), ImVec2(W, py0 - 1 * k), UIArt::BRASS_LIGHT, std::max(1.f, 1.2f * k));
	for (float x : {inner_l - 2 * k, inner_r + 2 * k}) {
		UIArt::rivet(draw, ImVec2(x, py0 + P * 0.3f), 3.2f * k);
		UIArt::rivet(draw, ImVec2(x, py0 + P * 0.75f), 3.2f * k);
	}
	// the three quick slots sit in a raised cartouche, the bar's centrepiece
	{
		// standing out as far below the plank as above it, so the slots are at its centre
		const ImVec2 ca(quick_l - 14 * k, py0 - 6 * k), cb(quick_r + 14 * k, py1 + 6 * k);
		UIArt::wood_panel(draw, ca, cb, (cb.y - ca.y) / 2, 0.25f);
		draw->AddRect(ca, cb, UIArt::BRASS, (cb.y - ca.y) / 2, 0, 2.5f * k);
		draw->AddRect(ImVec2(ca.x + 3 * k, ca.y + 3 * k), ImVec2(cb.x - 3 * k, cb.y - 3 * k), IM_COL32(255, 226, 160, 60), (cb.y - ca.y) / 2 - 3 * k, 0, 1);
		UIArt::rivet(draw, ImVec2(ca.x + 9 * k, (ca.y + cb.y) / 2), 3 * k);
		UIArt::rivet(draw, ImVec2(cb.x - 9 * k, (ca.y + cb.y) / 2), 3 * k);
	}
	// a few leaves growing out from behind the medallions
	UIArt::leaf(draw, ImVec2(orb_l.x + R * 0.55f, orb_l.y - R * 0.75f), 26 * k, -0.4f, IM_COL32(86, 140, 58, 255));
	UIArt::leaf(draw, ImVec2(orb_l.x + R * 0.7f, orb_l.y - R * 0.6f), 20 * k, 0.15f, IM_COL32(110, 168, 70, 255));
	UIArt::leaf(draw, ImVec2(orb_r.x - R * 0.55f, orb_r.y - R * 0.75f), 26 * k, 3.14159f + 0.4f, IM_COL32(86, 140, 58, 255));
	UIArt::leaf(draw, ImVec2(orb_r.x - R * 0.7f, orb_r.y - R * 0.6f), 20 * k, 3.14159f - 0.15f, IM_COL32(110, 168, 70, 255));

	// --- the plank's buttons ---
	if (begin_hit_area("##barplank", ImVec2(inner_l - 4 * k, py0), ImVec2(inner_r + 4 * k, py1))) {
		const float cy = (py0 + py1) / 2;
		// The name and how the Norn is, carved into the left of the plank, below the dock's
		// faces: its life stage, its life force by a heart and its strongest need by its sign.
		if (name_w > 60 * k) {
			const float size = ImGui::GetFontSize() * std::max(1.f, k / UIStyle::scale());
			const float name_y = py0 + 22 * k, line_y = name_y + size * 1.3f;
			if (c) {
				const std::string name = fit_text(short_name(*c), size * 1.15f, name_w - 8 * k);
				UIArt::text(draw, ImVec2(inner_l + 6 * k, name_y), size * 1.15f, UIArt::INK, name.c_str());
				const float small = size * 0.92f, right = inner_l + name_w;
				float x = inner_l + 6 * k;
				auto part = [&](UIArt::Glyph glyph, ImU32 colour, const std::string& text) {
					const float tw = ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0, text.c_str()).x;
					if (x + small + 3 * k + tw > right) {
						x = right; // no room for the rest
						return;
					}
					UIArt::glyph(draw, glyph, ImVec2(x + small / 2, line_y + small / 2), small, colour);
					x += small + 3 * k;
					UIArt::text(draw, ImVec2(x, line_y), small, UIArt::INK_SOFT, text.c_str());
					x += tw + 10 * k;
				};
				if (c->dead) {
					part(UIArt::Glyph::Dead, UIArt::INK_SOFT, "Dead");
				} else {
					const int32_t health = creature_health_percent(*c);
					part(c->asleep ? UIArt::Glyph::Sleepy : UIArt::Glyph::Dot, UIArt::INK_SOFT, c->asleep ? "Asleep" : stage_name(c->age));
					part(UIArt::Glyph::Heart, ImGui::ColorConvertFloat4ToU32(UIStyle::health_colour(static_cast<float>(health) / 100.f)), fmt::format("{}%", health));
					const size_t strongest = static_cast<size_t>(std::max_element(c->loci.drives.begin(), c->loci.drives.end()) - c->loci.drives.begin());
					if (c->loci.drives[strongest] > 96) {
						part(UIArt::drive_glyph(strongest), IM_COL32(255, 190, 120, 255), Kits::chemical_name(strongest + 1));
					}
				}
				ImGui::SetCursorScreenPos(ImVec2(inner_l, py0 + 6 * k));
				ImGui::InvisibleButton("##namearea", ImVec2(name_w, P - 12 * k));
				if (ImGui::IsItemClicked()) {
					Kits::open_pane(Kits::Pane::Norn, 0);
				}
				ImGui::SetItemTooltip("%s", creature_label(*c).c_str());
			} else {
				UIArt::text(draw, ImVec2(inner_l + 6 * k, name_y), size, UIArt::INK_SOFT, Kits::is_open("Hatchery") ? "Choose an egg" : "No Norn chosen");
			}
		}
		// the panes
		using UIArt::Picture;
		const struct {
			Kits::Pane pane;
			Picture picture;
			const char* tip;
		} panes[] = {
			{Kits::Pane::Norn, Picture::Norn, "Norn"},
			{Kits::Pane::Lab, Picture::Lab, "Lab"},
			{Kits::Pane::Objects, Picture::Objects, "Objects"},
			{Kits::Pane::Records, Picture::Records, "Records"},
		};
		float x = quick_l - gap - (fold_panes ? stud : panes_w) + stud / 2;
		if (fold_panes) {
			if (UIArt::round_button(draw, "##panes", ImVec2(x, cy), r, Picture::Records, "The panes", Kits::current_pane() != Kits::Pane::None)) {
				ImGui::OpenPopup("##panesmenu");
			}
			if (ImGui::BeginPopup("##panesmenu")) {
				for (const auto& p : panes) {
					if (ImGui::MenuItem(Kits::pane_name(p.pane), nullptr, Kits::current_pane() == p.pane)) {
						Kits::open_pane(p.pane, 0);
					}
				}
				ImGui::EndPopup();
			}
		} else {
			for (const auto& p : panes) {
				if (UIArt::round_button(draw, Kits::pane_name(p.pane), ImVec2(x, cy), r, p.picture, p.tip, Kits::current_pane() == p.pane)) {
					if (Kits::current_pane() == p.pane) {
						Kits::open_pane(Kits::Pane::None);
					} else {
						Kits::open_pane(p.pane, 0);
					}
				}
				x += stud;
			}
		}
		// the three quick slots
		x = quick_l + Q;
		const struct {
			Tray tray;
			Picture picture;
			const char* tip;
		} quick[] = {
			{Tray::Give, Picture::Give, "Give (1)"},
			{Tray::Heal, Picture::Heal, "Heal (2)"},
			{Tray::Teach, Picture::Teach, "Teach (3)"},
		};
		for (size_t i = 0; i < std::size(quick); ++i) {
			const auto& q = quick[i];
			s_quick_x[i] = x;
			if (UIArt::round_button(draw, q.tip, ImVec2(x, cy), Q, q.picture, q.tip, s_tray == q.tray)) {
				toggle_tray(q.tray, x);
			}
			x += 2 * Q + gap;
		}
		// the studs
		x = quick_r + gap + stud / 2;
		if (!fold_studs) {
			if (UIArt::round_button(draw, "##follow", ImVec2(x, cy), r, Picture::Follow, "Follow (F)", s_options.track_norn)) {
				s_options.track_norn = !s_options.track_norn;
			}
			x += stud;
			if (UIArt::round_button(draw, "##hatch", ImVec2(x, cy), r, Picture::Egg, "Hatchery (H)", Kits::is_open("Hatchery"))) {
				Kits::toggle("Hatchery");
			}
			x += stud;
			if (UIArt::round_button(draw, "##places", ImVec2(x, cy), r, Picture::Places, "Favourite places")) {
				ImGui::OpenPopup("##placesmenu");
			}
			x += stud;
			if (UIArt::round_button(draw, "##familytree", ImVec2(x, cy), r, Picture::Tree, "Family tree")) {
				Family::open_tree();
			}
			x += stud;
		}
		if (UIArt::round_button(draw, "##more", ImVec2(x, cy), r, Picture::More, "More")) {
			ImGui::OpenPopup("##moremenu");
		}
		x += stud;
		if (privileged) {
			// privileged mode shows while it's on: a click turns it off
			ImGui::SetCursorScreenPos(ImVec2(x - r, cy - r));
			plushie_toggle(2 * r);
		}
		open_test_popup("##moremenu");
		draw_places_menu();
		draw_more_menu(fold_studs);
	}
	ImGui::End();

	// --- the other Norns, along the top edge of the plank: a dock, oldest on the left ---
	const float rr = 16 * k, step = 2 * rr + 9 * k;
	std::vector<Creature*> roster;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* creature = obj->as_creature()) {
			roster.push_back(creature);
		}
	}
	std::stable_sort(roster.begin(), roster.end(), [](const Creature* a, const Creature* b) { return a->age_ticks > b->age_ticks; });
	const float roster_l = inner_l + 4 * k, roster_r = quick_l - 20 * k;
	const float ry = py0 - 2 * k;
	static float s_dock_scroll = 0;
	if (!roster.empty()) {
		const float view_w = std::max(step, roster_r - roster_l);
		const float total_w = static_cast<float>(roster.size()) * step;
		const float max_scroll = std::max(0.f, total_w - view_w + 8 * k);
		s_dock_scroll = std::clamp(s_dock_scroll, 0.f, max_scroll);
		// the whole row takes the wheel: the faces and the wood they sit on
		const ImVec2 area_a(roster_l - 4 * k, ry - rr * 1.6f - 6 * k), area_b(roster_l + std::min(view_w, total_w) + 6 * k, ry + rr + 5 * k);
		if (begin_hit_area("##barroster", area_a, area_b)) {
			auto& io = ImGui::GetIO();
			const bool over = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
			// the wheel or a drag scrolls it when there are more Norns than fit
			if (over && io.MouseWheel != 0) {
				s_dock_scroll -= io.MouseWheel * step;
			}
			if (over && io.MouseWheelH != 0) {
				s_dock_scroll += io.MouseWheelH * step;
			}
			if (over && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6)) {
				s_dock_scroll -= io.MouseDelta.x;
			}
			s_dock_scroll = std::clamp(s_dock_scroll, 0.f, max_scroll);
			// Where more Norns lie beyond an end, a little wooden arrow says so (a click on it
			// scrolls a page), and the faces shrink away as they near it rather than being cut
			// off. The shrinking grows in with the scrolling, so nothing jumps.
			const float hidden_l = s_dock_scroll, hidden_r = max_scroll - s_dock_scroll;
			const float arrow_l = UIArt::ease(0x5d0c11u, hidden_l > 1 ? 1.f : 0.f), arrow_r = UIArt::ease(0x5d0c12u, hidden_r > 1 ? 1.f : 0.f);
			const float arrow_w = 13 * k;
			const float edge_l = area_a.x + arrow_w * arrow_l, edge_r = area_b.x - arrow_w * arrow_r;
			const float fade_l = std::min(step * 1.2f, hidden_l), fade_r = std::min(step * 1.2f, hidden_r);
			// like the Dock: faces near the pointer grow a little, and spread to make room
			const float mouse_x = io.MousePos.x;
			float x = roster_l - s_dock_scroll;
			for (size_t i = 0; i < roster.size(); ++i) {
				Creature& n = *roster[i];
				const float centre_base = x + step / 2;
				const float d = over ? (mouse_x - centre_base) / (step * 1.6f) : 10.f;
				const float swell = UIArt::ease(static_cast<unsigned>(n.uid.to_integral() * 2654435761u), std::exp(-d * d), 16);
				const float grow = 1 + 0.3f * swell;
				const float width = step * (1 + 0.22f * swell);
				const ImVec2 cc(x + width / 2, ry - rr * 0.35f * swell);
				x += width;
				float shrink = 1;
				if (fade_l > 1) {
					shrink = std::min(shrink, std::clamp((cc.x - edge_l) / fade_l, 0.f, 1.f));
				}
				if (fade_r > 1) {
					shrink = std::min(shrink, std::clamp((edge_r - cc.x) / fade_r, 0.f, 1.f));
				}
				if (shrink < 0.08f || cc.x < area_a.x || cc.x > area_b.x) {
					continue;
				}
				const float radius = rr * grow * shrink;
				ImGui::PushID(&n);
				ImGui::SetCursorScreenPos(ImVec2(cc.x - rr * shrink, cc.y - rr * shrink));
				if (ImGui::InvisibleButton("face", ImVec2(rr * 2 * shrink, rr * 2 * shrink)) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6)) {
					select_creature(n);
				}
				const bool hovered = ImGui::IsItemHovered();
				if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					s_options.track_norn = true;
				}
				ImGui::SetItemTooltip("%s", creature_label(n).c_str());
				const bool chosen = n.uid == g_engine_context.world->current_norn;
				UIArt::brass_ring(draw, cc, radius + 3 * k * shrink, radius);
				draw_avatar(draw, n, cc, radius, chosen, n.genus == 1 ? Family::generation_of(n.moniker) : 0);
				ImGui::PopID();
			}
			// the arrows: a small wooden tab with a brass point
			const auto arrow = [&](const char* id, float grown, bool left) {
				if (grown < 0.05f) {
					return;
				}
				const float h = 24 * k * grown, w = arrow_w * grown;
				const float ax = left ? area_a.x : area_b.x - w;
				const ImVec2 a(ax, ry - h / 2), b(ax + w, ry + h / 2);
				UIArt::wood_panel(draw, a, b, 4 * k * grown, 0.1f);
				draw->AddRect(a, b, UIArt::BRASS_DARK, 4 * k * grown, 0, 1);
				const float tx = (a.x + b.x) / 2, th = h * 0.22f, tw = w * 0.26f;
				const ImVec2 p1(left ? tx - tw : tx + tw, ry), p2(left ? tx + tw : tx - tw, ry - th), p3(left ? tx + tw : tx - tw, ry + th);
				draw->AddTriangleFilled(p1, p2, p3, UIArt::BRASS_LIGHT);
				draw->AddTriangle(p1, p2, p3, UIArt::BRASS_DARK, 1);
				ImGui::SetCursorScreenPos(a);
				if (ImGui::InvisibleButton(id, ImVec2(std::max(1.f, w), std::max(1.f, h)))) {
					s_dock_scroll = std::clamp(s_dock_scroll + (left ? -1.f : 1.f) * std::max(step, view_w * 0.8f), 0.f, max_scroll);
				}
				ImGui::SetItemTooltip(left ? "More Norns: older ones" : "More Norns: younger ones");
			};
			arrow("##dockleft", arrow_l, true);
			arrow("##dockright", arrow_r, false);
		}
		ImGui::End();
	}

	// --- the portrait medallion ---
	if (begin_hit_area("##barportrait", ImVec2(orb_l.x - R, orb_l.y - R), ImVec2(orb_l.x + R, orb_l.y + R))) {
		ImGui::SetCursorScreenPos(ImVec2(orb_l.x - R, orb_l.y - R));
		if (ImGui::InvisibleButton("##portrait", ImVec2(2 * R, 2 * R))) {
			Kits::open_pane(Kits::Pane::Norn, 0);
		}
		if (c) {
			const int32_t health = c->dead ? 0 : creature_health_percent(*c);
			ImGui::SetItemTooltip("%s \xc2\xb7 %d%%%s", creature_label(*c).c_str(), health, c->asleep ? " \xc2\xb7 asleep" : "");
		} else {
			ImGui::SetItemTooltip("No Norn chosen");
		}
	}
	ImGui::End();
	UIArt::shadow_circle(draw, orb_l, R);
	UIArt::brass_ring(draw, orb_l, R, R * 0.84f);
	UIArt::glass(draw, orb_l, R * 0.84f);
	if (c) {
		const float inner = R * 1.25f;
		UIImages::draw_creature_face(draw, *c, ImVec2(orb_l.x - inner / 2, orb_l.y - inner / 2), ImVec2(orb_l.x + inner / 2, orb_l.y + inner / 2), c->dead ? IM_COL32(150, 150, 150, 170) : IM_COL32_WHITE);
		const int32_t health = c->dead ? 0 : creature_health_percent(*c);
		const ImVec4 hc = UIStyle::health_colour(static_cast<float>(health) / 100.f);
		ImU32 ring = ImGui::ColorConvertFloat4ToU32(hc);
		if (!c->dead && health < 20) {
			// a gentle pulse: it needs looking after
			const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.f);
			ring = (ring & ~IM_COL32_A_MASK) | (static_cast<ImU32>(255 * pulse) << IM_COL32_A_SHIFT);
		}
		UIArt::ring_meter(draw, orb_l, R * 0.77f, 5 * k, static_cast<float>(health) / 100.f, ring);
		// the generation, on a brass coin at the top left
		const int generation = c->genus == 1 ? Family::generation_of(c->moniker) : 0;
		if (generation > 0) {
			const ImVec2 coin(orb_l.x - R * 0.7f, orb_l.y - R * 0.7f);
			const float cr = R * 0.27f;
			UIArt::brass_ring(draw, coin, cr, cr * 0.7f);
			draw->AddCircleFilled(coin, cr * 0.7f, IM_COL32(74, 73, 75, 255), 20);
			const std::string g = std::to_string(generation);
			UIArt::text(draw, ImVec2(coin.x - ImGui::CalcTextSize(g.c_str()).x * 0.55f, coin.y - ImGui::GetFontSize() * 0.55f), ImGui::GetFontSize() * 1.1f, IM_COL32_WHITE, g.c_str());
		}
	} else {
		UIArt::picture(draw, UIArt::Picture::Egg, orb_l, R * 0.9f);
	}
	UIArt::glass_shine(draw, orb_l, R * 0.84f);

	// --- the porthole: Creature's View ---
	if (begin_hit_area("##barporthole", ImVec2(orb_r.x - R, orb_r.y - R), ImVec2(orb_r.x + R, orb_r.y + R))) {
		ImGui::SetCursorScreenPos(ImVec2(orb_r.x - R, orb_r.y - R));
		auto* it = c ? g_engine_context.objects->try_get(c->attention) : nullptr;
		if (ImGui::InvisibleButton("##porthole", ImVec2(2 * R, 2 * R)) && it) {
			look_at(it);
		}
		if (c) {
			ImGui::SetItemTooltip("%s", it ? fmt::format("{} is looking at: {}", short_name(*c), object_label(it)).c_str() : "Creature's View");
		}
	}
	ImGui::End();
	UIArt::shadow_circle(draw, orb_r, R);
	UIArt::brass_ring(draw, orb_r, R, R * 0.84f);
	for (int i = 0; i < 8; ++i) {
		const float a = static_cast<float>(i) * 3.14159265f / 4 + 0.39f;
		UIArt::rivet(draw, ImVec2(orb_r.x + std::cos(a) * R * 0.92f, orb_r.y + std::sin(a) * R * 0.92f), 2.6f * k);
	}
	UIArt::glass(draw, orb_r, R * 0.84f);
	const float pr = R * 0.84f;
	if (!(c && s_eye_placed && UIImages::creatures_view().draw(draw, ImVec2(orb_r.x - pr, orb_r.y - pr), ImVec2(orb_r.x + pr, orb_r.y + pr), pr))) {
		UIArt::picture(draw, UIArt::Picture::Follow, orb_r, R * 0.7f);
	}
	UIArt::glass_shine(draw, orb_r, pr);

	s_tray_bottom = ry - rr - 10 * k;
	return P + m;
}

// --- the trays ---

// a tray rises from the bar: as tall as what's in it, up to the room above the bar
float tray_max_height() {
	return std::max(160.f, s_tray_bottom - ImGui::GetFrameHeight() * 2.6f);
}

float s_tray_anim = 1; // 0 just opened, 1 settled (draw_trays)
bool s_tray_clipped = false; // begin_tray's clip, for end_tray

// fades what was drawn since a vertex count by a factor
void fade_since(ImDrawList* draw, int first, float alpha) {
	for (int i = first; i < draw->VtxBuffer.Size; ++i) {
		ImU32& col = draw->VtxBuffer[i].col;
		const ImU32 a = static_cast<ImU32>(static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
		col = (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
	}
}

bool begin_tray(const char* id, const char* title, float width) {
	auto& io = ImGui::GetIO();
	const float k = bar_scale();
	width = std::min(width, io.DisplaySize.x - 16 * k);
	const float x = std::clamp(s_tray_x - width / 2, 8 * k, io.DisplaySize.x - width - 8 * k);
	// it rises a little out of the bar as it opens, and fades in
	ImGui::SetNextWindowPos(ImVec2(x, s_tray_bottom + (1 - s_tray_anim) * 18 * k), 0, ImVec2(0, 1));
	ImGui::SetNextWindowSizeConstraints(ImVec2(width, 80 * k), ImVec2(width, tray_max_height()));
	const float border = 12 * k;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(border + 8 * k, border + 14 * k));
	// buttons of dark wood with a brass edge, brass when pressed
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9 * k);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.21f, 0.13f, 1));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.43f, 0.30f, 0.17f, 1));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.73f, 0.56f, 0.25f, 1));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.73f, 0.56f, 0.25f, 0.55f));
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.05f, s_tray_anim));
	const bool open = ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize);
	if (open) {
		const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
		auto* draw = ImGui::GetWindowDrawList();
		draw->PushClipRectFullScreen();
		const int first = draw->VtxBuffer.Size;
		UIArt::window_frame(draw, pos, ImVec2(pos.x + size.x, pos.y + size.y), border, title);
		fade_since(draw, first, s_tray_anim);
		draw->PopClipRect();
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			s_tray = Tray::None;
		}
		// what's in it stays inside the frame as it scrolls
		ImGui::PushClipRect(ImVec2(pos.x + border, pos.y + border), ImVec2(pos.x + size.x - border, pos.y + size.y - border), true);
		s_tray_clipped = true;
	}
	return open;
}

void end_tray() {
	if (s_tray_clipped) {
		ImGui::PopClipRect();
		s_tray_clipped = false;
	}
	ImGui::End();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);
	ImGui::PopStyleVar(3);
}

// the largest card (up to most) that fits count of them in the room, but never smaller than
// least: past that the tray scrolls instead
float card_size(size_t count, float width, float height, float most, float least) {
	for (float card = most; card > least; card -= 4) {
		const int columns = std::max(1, static_cast<int>((width + 6) / (card + 6)));
		const int rows = static_cast<int>((count + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
		if (static_cast<float>(rows) * (card + 6) <= height) {
			return card;
		}
	}
	return least;
}

// a label in up to two lines, word by word, the second cut short to fit
std::vector<std::string> card_label(const std::string& text, float size, float width) {
	ImFont* font = ImGui::GetFont();
	auto fits = [&](const std::string& s) { return font->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x <= width; };
	if (fits(text)) {
		return {text};
	}
	// as many whole words as fit on the first line
	std::string first;
	size_t at = 0;
	while (at < text.size()) {
		const size_t space = text.find(' ', at);
		const std::string word = text.substr(at, space == std::string::npos ? std::string::npos : space - at);
		const std::string trial = first.empty() ? word : first + " " + word;
		if (!fits(trial)) {
			break;
		}
		first = trial;
		at = space == std::string::npos ? text.size() : space + 1;
	}
	if (first.empty()) {
		return {fit_text(text, size, width)};
	}
	return {first, fit_text(text.substr(at), size, width)};
}

// the next item on this line if it fits, else on the next
void flow(float width) {
	const float right = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width;
	if (right < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) {
		ImGui::SameLine();
	}
}

// picture cards in a grid; returns the one clicked, or -1
int item_grid(const std::vector<Kits::QuickItem>& items, float card) {
	int clicked = -1;
	const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + 6) / (card + 6)));
	auto* draw = ImGui::GetWindowDrawList();
	for (size_t i = 0; i < items.size(); ++i) {
		const auto& item = items[i];
		if (i % static_cast<size_t>(columns) != 0) {
			ImGui::SameLine(0, 6);
		}
		ImGui::PushID(static_cast<int>(i));
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::BeginDisabled(item.remaining == 0);
		if (ImGui::InvisibleButton("##card", ImVec2(card, card))) {
			clicked = static_cast<int>(i);
		}
		const bool hovered = ImGui::IsItemHovered();
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("%s%s%s", item.name.c_str(), item.description.empty() ? "" : "\n", item.description.c_str());
		draw->AddRectFilled(p, ImVec2(p.x + card, p.y + card), hovered ? IM_COL32(96, 68, 40, 255) : IM_COL32(58, 42, 28, 255), 10);
		draw->AddRect(p, ImVec2(p.x + card, p.y + card), hovered ? UIArt::BRASS_LIGHT : IM_COL32(186, 142, 64, 130), 10, 0, hovered ? 2.f : 1.f);
		// the name, in a smaller hand, on a line or two at the bottom
		const float size = ImGui::GetFontSize() * 0.86f;
		const auto lines = card_label(item.name, size, card - 8);
		const float label_h = size * 1.15f * static_cast<float>(lines.size()) + 4;
		if (item.picture) {
			// as big as fits above the name (never spilling out of the card), at most 3 times
			const float fit = std::min((card - 10) / static_cast<float>(item.picture->width), (card - label_h - 8) / static_cast<float>(item.picture->height));
			const float sc = std::min(fit, 3.f);
			const float w = static_cast<float>(item.picture->width) * sc, h = static_cast<float>(item.picture->height) * sc;
			UIImages::draw_bitmap(draw, item.picture, ImVec2(p.x + (card - w) / 2, p.y + (card - label_h - h) / 2 + 2), sc);
		}
		float y = p.y + card - label_h;
		for (const auto& line : lines) {
			const float tw = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, line.c_str()).x;
			draw->AddText(ImGui::GetFont(), size, ImVec2(p.x + (card - tw) / 2, y), item.remaining == 0 ? IM_COL32(150, 130, 110, 160) : UIArt::INK, line.c_str());
			y += size * 1.15f;
		}
		if (item.remaining >= 0) {
			const std::string left = std::to_string(item.remaining);
			const ImVec2 bs = ImGui::CalcTextSize(left.c_str());
			const ImVec2 b(p.x + card - bs.x - 10, p.y + 4);
			draw->AddRectFilled(ImVec2(b.x - 4, b.y), ImVec2(b.x + bs.x + 4, b.y + bs.y + 2), IM_COL32(30, 20, 12, 220), 6);
			draw->AddText(b, UIArt::INK_SOFT, left.c_str());
		}
		ImGui::PopID();
	}
	return clicked;
}

void draw_give_tray() {
	auto* c = selected_creature();
	const float k = bar_scale();
	const std::string title = c ? "Give to " + short_name(*c) : std::string("Give");
	if (begin_tray("##givetray", title.c_str(), 600 * k)) {
		auto items = Kits::injector_items(true);
		if (items.empty()) {
			UIArt::glyph_text(UIArt::Glyph::Dot, UIArt::INK_SOFT, "No add-ons (.cob files) in the game folder", ImGui::GetColorU32(ImGuiCol_TextDisabled));
		}
		const float room = tray_max_height() - ImGui::GetCursorPosY() - 30 * k;
		const float card = card_size(items.size(), ImGui::GetContentRegionAvail().x, room, 104 * k, 76 * k);
		const int clicked = item_grid(items, card);
		if (clicked >= 0 && Kits::add_item(items[static_cast<size_t>(clicked)])) {
			// add-ons place themselves as their makers meant: most go to their own spot in the
			// world, and things like food come in the hand for you to put down
			const bool in_hand = g_engine_context.objects->try_get(g_engine_context.pointer->m_edited_object) != nullptr;
			show_message(fmt::format("{} {}", items[static_cast<size_t>(clicked)].name, in_hand ? "is in the hand" : "is in the world"));
		}
	}
	end_tray();
}

void draw_heal_tray() {
	auto* c = selected_creature();
	const float k = bar_scale();
	const std::string title = c ? "Heal " + short_name(*c) : std::string("Heal");
	if (begin_tray("##healtray", title.c_str(), 640 * k)) {
		if (!c) {
			UIArt::glyph_text(UIArt::Glyph::Dot, UIArt::INK_SOFT, "Choose a Norn first", ImGui::GetColorU32(ImGuiCol_TextDisabled));
		} else {
			// how it is: its life force, awake or not, then its strongest needs by their signs
			const int32_t health = c->dead ? 0 : creature_health_percent(*c);
			const float details_w = UIArt::chip_width("Health");
			UIStyle::meter(fmt::format("{}%", health).c_str(), static_cast<float>(health) / 100.f, UIStyle::health_colour(static_cast<float>(health) / 100.f), ImGui::GetContentRegionAvail().x * 0.45f);
			ImGui::SetItemTooltip("Life force");
			ImGui::SameLine();
			UIArt::glyph_text(c->dead ? UIArt::Glyph::Dead : c->asleep ? UIArt::Glyph::Sleepy : UIArt::Glyph::Eye, UIArt::INK_SOFT, c->dead ? "Dead" : c->asleep ? "Asleep" : "Awake",
				ImGui::GetColorU32(ImGuiCol_TextDisabled));
			if (c->bacterium.state) {
				ImGui::SameLine();
				UIArt::glyph_text(UIArt::Glyph::Fear, IM_COL32(255, 120, 90, 255), "Ill", IM_COL32(255, 150, 120, 255));
			}
			// the whole of its health, in its Norn pane
			ImGui::SameLine(ImGui::GetContentRegionMax().x - details_w);
			if (UIArt::chip("##healthdetails", UIArt::Glyph::Chevron, "Health")) {
				Kits::open_pane(Kits::Pane::Norn, 1);
				s_tray = Tray::None;
			}
			std::vector<size_t> order(c->loci.drives.size());
			for (size_t i = 0; i < order.size(); ++i) {
				order[i] = i;
			}
			std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return c->loci.drives[a] > c->loci.drives[b]; });
			for (size_t i = 0; i < 4 && i < order.size(); ++i) {
				const uint8_t level = c->loci.drives[order[i]];
				if (level < 64) {
					break;
				}
				const std::string need = fmt::format("{} {}%", Kits::chemical_name(order[i] + 1), level * 100 / 255);
				if (i > 0) {
					flow(UIArt::chip_width(need.c_str()));
				}
				const ImU32 colour = ImGui::ColorConvertFloat4ToU32(UIStyle::health_colour(1.f - level / 255.f));
				UIArt::chip(fmt::format("##need{}", i).c_str(), UIArt::drive_glyph(order[i]), need.c_str(), IM_COL32(44, 30, 20, 255), colour);
			}
			ImGui::SeparatorText("Herbs");
			auto herbs = Kits::shop_items(false);
			const int clicked = item_grid(herbs, 84 * k);
			if (clicked >= 0 && Kits::add_item(herbs[static_cast<size_t>(clicked)])) {
				show_message(herbs[static_cast<size_t>(clicked)].name + " is in the hand");
			}
			ImGui::SeparatorText("Medicine");
			const auto& mixtures = Kits::injection_names();
			bool first = true;
			for (size_t i = 0; i < mixtures.size(); ++i) {
				if (mixtures[i].empty()) {
					continue;
				}
				if (!first) {
					flow(UIArt::chip_width(mixtures[i].c_str()));
				}
				first = false;
				ImGui::BeginDisabled(c->dead);
				if (UIArt::chip(fmt::format("##medicine{}", i).c_str(), UIArt::Glyph::Pill, mixtures[i].c_str()) && !c->dead) {
					Kits::inject(*c, i);
					show_message(fmt::format("{} for {}", mixtures[i], short_name(*c)));
				}
				ImGui::EndDisabled();
			}
		}
	}
	end_tray();
}

void draw_teach_tray() {
	auto* c = selected_creature();
	const float k = bar_scale();
	const std::string title = c ? "Teach " + short_name(*c) : std::string("Teach");
	if (begin_tray("##teachtray", title.c_str(), 600 * k)) {
		auto* draw = ImGui::GetWindowDrawList();
		// speak, reward, punish
		const float R = 26 * k;
		const ImVec2 start = ImGui::GetCursorScreenPos();
		if (UIArt::round_button(draw, "##speak", ImVec2(start.x + R, start.y + R), R, UIArt::Picture::Teach, "Speak (Enter)")) {
			s_touch_typing_requested = true;
		}
		if (UIArt::round_button(draw, "##reward", ImVec2(start.x + R * 3.4f, start.y + R), R, UIArt::Picture::Tickle, "Reward: a tickle", false, c != nullptr) && c) {
			touch_norn(*c, true);
		}
		if (UIArt::round_button(draw, "##punish", ImVec2(start.x + R * 5.8f, start.y + R), R, UIArt::Picture::Slap, "Punish: a slap", false, c != nullptr) && c) {
			touch_norn(*c, false);
		}
		// labels under the three buttons
		for (const auto& [x, label] : {std::pair<float, const char*>{R, "Speak"}, {R * 3.4f, "Reward"}, {R * 5.8f, "Punish"}}) {
			const float w = ImGui::CalcTextSize(label).x;
			UIArt::text(draw, ImVec2(start.x + x - w / 2, start.y + 2 * R + 3 * k), ImGui::GetFontSize(), UIArt::INK_SOFT, label);
		}
		// what it's looking at, and the word for it (from the world's blackboards) to say
		ImGui::SetCursorScreenPos(ImVec2(start.x + R * 7.4f, start.y + 4));
		ImGui::BeginGroup();
		if (!c) {
			UIArt::glyph_text(UIArt::Glyph::Dot, UIArt::INK_SOFT, "Choose a Norn first", ImGui::GetColorU32(ImGuiCol_TextDisabled));
		} else {
			auto* it = g_engine_context.objects->try_get(c->attention);
			UIArt::glyph_text(UIArt::Glyph::Eye, UIArt::INK_SOFT, it ? object_label(it).c_str() : "Nothing in particular", ImGui::GetColorU32(ImGuiCol_TextDisabled));
			ImGui::SetItemTooltip("What %s is looking at", short_name(*c).c_str());
			if (it) {
				const int32_t category = c->attention_category(*it);
				std::string noun;
				for (auto* obj : *g_engine_context.objects) {
					if (auto* board = obj->as_blackboard()) {
						for (const auto& w : board->words) {
							if (category > 0 && w.value == static_cast<uint32_t>(16 + category) && !w.text.empty()) {
								noun = w.text;
							}
						}
					}
				}
				if (!noun.empty() && UIArt::chip("##noun", UIArt::Glyph::Speak, noun.c_str())) {
					say(noun);
				}
				ImGui::SetItemTooltip("Say \"%s\"", noun.c_str());
			}
		}
		ImGui::EndGroup();
		// The action words, each with its sign, green once it knows the word: say one while it
		// does that, and it learns it.
		ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + 2 * R + ImGui::GetTextLineHeight() + 14 * k));
		ImGui::Separator();
		ImGui::Spacing();
		using UIArt::Glyph;
		static const char* verbs[] = {"push", "pull", "stop", "come", "run", "get", "drop", "rest", "west", "east", "stay", "why"};
		static const Glyph signs[] = {Glyph::Push, Glyph::Pull, Glyph::Stop, Glyph::Come, Glyph::Run, Glyph::Get, Glyph::Drop, Glyph::Rest, Glyph::West, Glyph::East, Glyph::Stay, Glyph::Why};
		static const size_t index[] = {1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 0, 8};
		// all the same width, the widest word's, so they line up
		float width = 0;
		for (const char* verb : verbs) {
			width = std::max(width, UIArt::chip_width(verb));
		}
		width += 4 * k;
		for (size_t i = 0; i < std::size(verbs); ++i) {
			bool known = false;
			std::string says;
			if (c && index[i] < c->vocabulary.size()) {
				const auto& v = c->vocabulary[index[i]];
				says = v.out;
				known = !v.out.empty() && std::equal(v.out.begin(), v.out.end(), verbs[i], verbs[i] + std::strlen(verbs[i]), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }) && v.out.size() == std::strlen(verbs[i]);
			}
			if (i > 0) {
				flow(width);
			}
			ImGui::BeginDisabled(!c);
			if (UIArt::chip(fmt::format("##verb{}", i).c_str(), signs[i], verbs[i], known ? IM_COL32(70, 104, 40, 255) : 0, UIArt::INK, width) && c) {
				say(verbs[i]);
			}
			ImGui::EndDisabled();
			if (c) {
				if (known) {
					// a tick in the corner
					const ImVec2 b = ImGui::GetItemRectMax();
					const float h = ImGui::GetFrameHeight();
					UIArt::glyph(ImGui::GetWindowDrawList(), Glyph::Check, ImVec2(b.x - h * 0.42f, ImGui::GetItemRectMin().y + h / 2), h * 0.42f, IM_COL32(190, 240, 150, 255));
				}
				ImGui::SetItemTooltip(known ? "%s knows \"%s\"" : "%s says \"%s\" for this", short_name(*c).c_str(), known ? verbs[i] : says.c_str());
			}
		}
		// and its name
		if (c) {
			ImGui::Spacing();
			const std::string name = short_name(*c);
			if (UIArt::chip("##sayname", Glyph::Tag, name.c_str())) {
				say(name);
			}
			ImGui::SetItemTooltip("Say its name");
		}
	}
	end_tray();
}

void draw_trays() {
	if (s_options.classic_layout) {
		s_tray = Tray::None;
		return;
	}
	static Tray shown = Tray::None;
	static double opened = 0;
	if (s_tray != shown) {
		shown = s_tray;
		opened = ImGui::GetTime();
	}
	const float t = std::clamp(static_cast<float>((ImGui::GetTime() - opened) / 0.18), 0.f, 1.f);
	s_tray_anim = t * t * (3 - 2 * t);
	switch (s_tray) {
		case Tray::Give: draw_give_tray(); break;
		case Tray::Heal: draw_heal_tray(); break;
		case Tray::Teach: draw_teach_tray(); break;
		case Tray::None: break;
	}
}

bool creatures_view_on();

void draw_porthole(float bottom) {
	// Creature's View: the creature's eye on the world (batch 4, Q11)
	auto* c = selected_creature();
	if (!creatures_view_on() || !c || !s_eye_placed) {
		return;
	}
	auto* it = g_engine_context.objects->try_get(c->attention);
	const int32_t left = static_cast<int32_t>(s_eye_x) - EYE_WIDTH / 2;
	const int32_t top = std::clamp(static_cast<int32_t>(s_eye_y) - EYE_HEIGHT / 2, 0, CREATURES1_WORLD_HEIGHT - EYE_HEIGHT);
	UIImages::creatures_view().look_at(Rect2i{left, top, EYE_WIDTH, EYE_HEIGHT}, EYE_WIDTH, EYE_HEIGHT);
	// the modern layout shows it in its bottom panel (draw_hud); the original, in a window of
	// its own at its own size
	if (!s_options.classic_layout) {
		return;
	}
	const float zoom = 1.f;
	ImGui::SetNextWindowPos(ImVec2(HUD_PAD, ImGui::GetIO().DisplaySize.y - bottom - HUD_PAD), ImGuiCond_FirstUseEver, ImVec2(0, 1));
	const std::string title = fmt::format("Creature's View: {}###porthole", short_name(*c));
	if (ImGui::Begin(title.c_str(), &s_options.creatures_view, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
		UIStyle::window_shadow();
		ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = EYE_WIDTH * zoom, h = EYE_HEIGHT * zoom;
		UIImages::creatures_view().image(ImVec2(w, h));
		if (!s_options.classic_layout) {
			ImGui::GetWindowDrawList()->AddRect(pos, ImVec2(pos.x + w, pos.y + h), UIStyle::accent(), 10, 0, 3);
			ImGui::TextUnformatted(it ? object_label(it).c_str() : "Nothing in particular");
			if (it) {
				ImGui::SameLine();
				if (ImGui::SmallButton("Look")) {
					look_at(it);
				}
			}
		}
	}
	ImGui::End();
}

// --- crash reports and the debug console ---

std::string s_crash_report;
std::string s_crash_report_path;
bool s_checked_for_crash = false;

void open_in_file_browser(const std::string& path) {
	std::string url = "file://";
	for (char ch : path) {
		if (ch == ' ') {
			url += "%20";
		} else if (ch == '\\') {
			url += '/';
		} else {
			url += ch;
		}
	}
	SDL_OpenURL(url.c_str());
}

void draw_crash_windows() {
	// a crash in the last session: offer the report
	if (!s_checked_for_crash) {
		s_checked_for_crash = true;
		s_crash_report = CrashReport::take_unseen_report(&s_crash_report_path);
		s_show_crash_window = !s_crash_report.empty();
	}
	if (s_show_crash_window) {
		ImGui::SetNextWindowSize(ImVec2(620, 440), ImGuiCond_Appearing);
		if (ImGui::Begin("Crash reports and logs", &s_show_crash_window, ImGuiWindowFlags_NoSavedSettings)) {
			if (!s_crash_report.empty()) {
				ImGui::TextWrapped("Creatures crashed last time, sorry! This report says what happened. Please send it along so the problem can be fixed.");
				ImGui::TextDisabled("%s", s_crash_report_path.c_str());
				if (ImGui::Button("Copy the report")) {
					ImGui::SetClipboardText(s_crash_report.c_str());
				}
				ImGui::SameLine();
			} else {
				ImGui::TextWrapped("If the game crashes, a report is saved here. The log of each session is kept too.");
			}
			if (ImGui::Button("Open the reports folder")) {
				std::error_code err;
				std::filesystem::create_directories(CrashReport::reports_folder(), err);
				open_in_file_browser(CrashReport::reports_folder());
			}
			ImGui::SameLine();
			if (ImGui::Button("Open this session's log")) {
				open_in_file_browser(CrashReport::session_log_path());
			}
			ImGui::TextDisabled("Build %s", CrashReport::build_id());
			if (!s_crash_report.empty()) {
				ImGui::BeginChild("##report", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
				ImGui::TextUnformatted(s_crash_report.c_str());
				ImGui::EndChild();
			}
		}
		ImGui::End();
	}

	// the debug console: the live log and what the engine is doing
	if (s_show_debug_console) {
		ImGui::SetNextWindowSize(ImVec2(680, 420), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Debug console", &s_show_debug_console)) {
			size_t scripts = 0;
			for (auto it = g_engine_context.macros->begin(); it != g_engine_context.macros->end(); ++it) {
				scripts++;
			}
			size_t objects = 0;
			for (auto* obj : *g_engine_context.objects) {
				(void)obj;
				objects++;
			}
			ImGui::Text("Tick %u   objects %zu   scripts running %zu   %.0f fps   build %s", g_engine_context.world->tick, objects, scripts, ImGui::GetIO().Framerate, CrashReport::build_id());
			if (ImGui::BeginTabBar("##debugtabs")) {
				if (ImGui::BeginTabItem("Log")) {
					static char filter[64] = "";
					ImGui::SetNextItemWidth(200);
					ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter));
					ImGui::SameLine();
					static bool follow = true;
					ImGui::Checkbox("Follow", &follow);
					ImGui::SameLine();
					auto lines = CrashReport::recent_log();
					if (ImGui::Button("Copy")) {
						std::string all;
						for (auto& l : lines) {
							all += l + "\n";
						}
						ImGui::SetClipboardText(all.c_str());
					}
					ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
					for (auto& l : lines) {
						if (filter[0] && l.find(filter) == std::string::npos) {
							continue;
						}
						const bool bad = l.rfind("error", 0) == 0 || l.find("WARN") != std::string::npos;
						if (bad) {
							ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "%s", l.c_str());
						} else {
							ImGui::TextUnformatted(l.c_str());
						}
					}
					if (follow) {
						ImGui::SetScrollHereY(1.0f);
					}
					ImGui::EndChild();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Scripts")) {
					ImGui::BeginChild("##scripts", ImVec2(0, 0), ImGuiChildFlags_Borders);
					for (auto it = g_engine_context.macros->begin(); it != g_engine_context.macros->end(); ++it) {
						const Macro& m = *it;
						auto* owner = g_engine_context.objects->try_get(m.ownr);
						ImGui::Text("%3u %3u %3u ev %-3d %s", m.classifier[0], m.classifier[1], m.classifier[2], m.event, owner ? object_label(owner).c_str() : "?");
						ImGui::SameLine();
						ImGui::TextDisabled("%s", m.script.substr(m.ip, 60).c_str());
					}
					ImGui::EndChild();
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
		}
		ImGui::End();
	}
}

void draw_error_details() {
	if (!s_show_error_details) {
		return;
	}
	ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
	if (ImGui::Begin("A script stopped", &s_show_error_details, ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::TextWrapped("%s", s_error_details.c_str());
		ImGui::TextDisabled("The script was stopped and the game carried on, as in the original.");
		if (ImGui::Button("Copy")) {
			ImGui::SetClipboardText(s_error_details.c_str());
		}
	}
	ImGui::End();
}

void next_creature(int direction) {
	std::vector<Creature*> all;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature()) {
			all.push_back(c);
		}
	}
	if (all.empty()) {
		return;
	}
	auto current = std::find(all.begin(), all.end(), selected_creature());
	long i = current == all.end() ? 0 : (current - all.begin()) + direction;
	i = (i % static_cast<long>(all.size()) + static_cast<long>(all.size())) % static_cast<long>(all.size());
	select_creature(*all[static_cast<size_t>(i)]);
}

// --- markers drawn over the world ---

void draw_world_markers() {
	auto* draw = ImGui::GetBackgroundDrawList();
	auto& viewport = *g_engine_context.viewport;
	auto rect = [&](Rect2f b, ImU32 colour, float thickness) {
		float x = viewport.world_x_to_window_x(b.x);
		float y = viewport.world_y_to_window_y(b.y);
		draw->AddRect(ImVec2(x, y), ImVec2(x + b.width * viewport.scale, y + b.height * viewport.scale), colour, 0, 0, thickness);
	};
	auto* c = selected_creature();

	// "Which is my norn?"
	if (c && ImGui::GetTime() < s_which_is_my_norn_until) {
		auto b = c->get_bbox();
		// a 2 px pink outline (batch 4, Q23)
		rect(Rect2f{b.x - 4, b.y - 4, b.width + 8, b.height + 8}, IM_COL32(255, 128, 255, 255), 2);
	}
}

// Track Norn (batch 4, Q17): keep the selected creature horizontally centred with its
// ground point at 5/8 of the view's height, unless the hand is holding it. Scrolling by hand
// pauses it until the creature has been back inside the central box for 20 ticks.
float s_track_vx = 0, s_track_vy = 0;
int32_t s_track_paused_ticks = -1; // -1: not paused
constexpr float TRACK_MAX_SPEED = 25;

void track_norn_tick() {
	auto* c = selected_creature();
	auto& viewport = *g_engine_context.viewport;
	if (!s_options.track_norn || !c || c->movement_status == MOVEMENT_MOUSEDRIVEN) {
		s_track_vx = s_track_vy = 0;
		return;
	}
	auto camera = to_rectf(viewport.get_main_camera_rect());
	// where the creature is in the view, the short way round the world
	float rx = c->ground_x() - camera.x;
	if (rx > CREATURES1_WORLD_WIDTH / 2) {
		rx -= CREATURES1_WORLD_WIDTH;
	} else if (rx < -CREATURES1_WORLD_WIDTH / 2) {
		rx += CREATURES1_WORLD_WIDTH;
	}
	const float ry = c->ground_y() - camera.y;
	const bool in_box = rx >= camera.width / 8 && rx <= camera.width * 7 / 8 && ry >= camera.height * 3 / 8 && ry <= camera.height * 7 / 8;
	if (viewport.manually_scrolling()) {
		s_track_paused_ticks = 0;
		return;
	}
	if (s_track_paused_ticks >= 0) {
		s_track_paused_ticks = in_box ? s_track_paused_ticks + 1 : 0;
		if (s_track_paused_ticks < 20) {
			return;
		}
		s_track_paused_ticks = -1;
	}
	const float dx = rx - camera.width / 2;
	const float dy = ry - camera.height * 5 / 8;
	if (!viewport.smooth_scrolling) {
		// jump once it leaves the central box
		if (!in_box) {
			viewport.scroll_by(static_cast<int32_t>(dx), static_cast<int32_t>(dy));
		}
		return;
	}
	// speed builds up by 3 a tick while it's far from the aim, is capped at 25 and decays by
	// 1 a tick; the view moves by speed / 3 px a tick
	auto step = [](float& v, float off, float threshold) {
		if (std::abs(off) > threshold) {
			v += off > 0 ? 3 : -3;
		}
		v = std::clamp(v, -TRACK_MAX_SPEED, TRACK_MAX_SPEED);
		v += v > 0 ? -1 : v < 0 ? 1 : 0;
	};
	step(s_track_vx, dx, camera.width / 4);
	step(s_track_vy, dy, camera.height / 6);
	viewport.scroll_by(static_cast<int32_t>(s_track_vx / 3), static_cast<int32_t>(s_track_vy / 3), true);
}

// Creature's View (batch 4, Q11): a 128 x 96 view centred on what the selected creature is
// attending to (while it's awake and has a target in the world), otherwise on the creature.
// It moves up to 16 px a tick in x and y, and jumps when the target is more than 640 px away
// or when it first opens. With no selected creature, or a dead one, it closes.

// Creature's View is always on in the modern layout; classic mode has the original's toggle
bool creatures_view_on() {
	return s_options.creatures_view || !s_options.classic_layout;
}

void creatures_view_tick() {
	auto* c = selected_creature();
	if (!creatures_view_on()) {
		s_eye_placed = false;
		return;
	}
	if (!c || c->dead) {
		// in classic mode it closes when the creature it was showing is no longer selected or
		// dies (batch 4, Q11)
		if (s_eye_placed && s_options.classic_layout) {
			s_options.creatures_view = false;
		}
		s_eye_placed = false;
		return;
	}
	float tx, ty;
	if (c->has_target && !c->asleep && c->target_y <= CREATURES1_WORLD_HEIGHT && g_engine_context.objects->try_get(c->attention)) {
		tx = c->target_x;
		ty = c->target_y;
	} else {
		auto b = c->get_bbox();
		tx = b.x + b.width / 2;
		ty = b.y + b.height / 2;
	}
	float dx = tx - s_eye_x;
	if (dx > CREATURES1_WORLD_WIDTH / 2) {
		dx -= CREATURES1_WORLD_WIDTH;
	} else if (dx < -CREATURES1_WORLD_WIDTH / 2) {
		dx += CREATURES1_WORLD_WIDTH;
	}
	if (!s_eye_placed || s_eye_creature != c->uid || std::abs(dx) > 640) {
		s_eye_x = tx;
		s_eye_y = ty;
		s_eye_placed = true;
		s_eye_creature = c->uid;
		return;
	}
	s_eye_x += std::clamp(dx, -16.f, 16.f);
	s_eye_y += std::clamp(ty - s_eye_y, -16.f, 16.f);
	if (s_eye_x < 0) {
		s_eye_x += CREATURES1_WORLD_WIDTH;
	} else if (s_eye_x >= CREATURES1_WORLD_WIDTH) {
		s_eye_x -= CREATURES1_WORLD_WIDTH;
	}
}

// --- dialogs ---

void draw_message_boxes() {
	// script errors: the script is stopped and the game keeps going (Q33)
	auto& errors = g_engine_context.macros->ctx.error_messages;
	// during a wolfling run nothing waits for the player: errors are only logged
	if (!s_options.show_script_errors || Wolfling::active()) {
		errors.clear();
	}
    // Script failures use the same nonmodal notification/details path in both
    // layouts. They must never require a modal Retry/OK to keep playing.
    for (auto& e : errors) toast("A script stopped working", {}, true, e);
    errors.clear();
	if (!s_message.empty()) {
		ImGui::OpenPopup("Creatures");
	}
	if (ImGui::BeginPopupModal("Creatures", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::PushTextWrapPos(420);
		ImGui::TextUnformatted(s_message.c_str());
		ImGui::PopTextWrapPos();
		if (ImGui::Button("OK", ImVec2(120, 0))) {
			s_message.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

// keys (and clicks and touches) as little keycaps side by side, as an ImGui item
void key_caps(std::initializer_list<const char*> keys) {
	auto* d = ImGui::GetWindowDrawList();
	const float h = ImGui::GetTextLineHeight() + 6;
	bool first = true;
	for (const char* key : keys) {
		if (!first) {
			ImGui::SameLine(0, 4);
		}
		first = false;
		const ImVec2 ts = ImGui::CalcTextSize(key);
		const float w = std::max(h, ts.x + 12);
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(w, h + 1));
		// the cap, on its darker edge
		d->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + w, p.y + h + 1), IM_COL32(16, 16, 18, 220), 5);
		d->AddRectFilled(p, ImVec2(p.x + w, p.y + h - 1), IM_COL32(70, 70, 76, 255), 5);
		d->AddRect(p, ImVec2(p.x + w, p.y + h - 1), IM_COL32(124, 124, 134, 255), 5);
		d->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + (h - 1 - ts.y) / 2), IM_COL32(236, 236, 240, 255), key);
	}
}

// a window in the middle of the screen, never bigger than it
void centre_window(float width = 0) {
	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(screen.x / 2, screen.y / 2), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(width > 0 ? std::min(width, screen.x - 24) : 0, 0), ImVec2(screen.x - 24, screen.y - 24));
}

void draw_help() {
#ifdef __APPLE__
	const char* ctrl = "Cmd";
#else
	const char* ctrl = "Ctrl";
#endif
	struct Row {
		std::initializer_list<const char*> keys;
		const char* does;
	};
	const float em = ImGui::GetFontSize();
	auto section = [em](const char* title, std::initializer_list<Row> rows) {
		ImGui::SeparatorText(title);
		if (ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed);
			for (const auto& row : rows) {
				ImGui::TableNextColumn();
				key_caps(row.keys);
				ImGui::TableNextColumn();
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
				ImGui::TextUnformatted(row.does);
			}
			ImGui::EndTable();
		}
	};
	// two columns of them when there's room, else one
	const bool two = ImGui::GetIO().DisplaySize.x > em * 48 && ImGui::BeginTable("##helpcolumns", 2, ImGuiTableFlags_SizingFixedFit);
	if (two) {
		ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthFixed, em * 21);
		ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthFixed, em * 22);
		ImGui::TableNextColumn();
	}
	section("The hand", {
		{{"Click"}, "Use \xc2\xb7 tickle a head \xc2\xb7 slap a body"},
		{{"Right-click"}, "Pick up \xc2\xb7 drop"},
		{{"\xe2\x86\x90", "\xe2\x86\x92", "\xe2\x86\x91", "\xe2\x86\x93"}, "Look around"},
		{{"Shift", "Wheel"}, "Zoom"},
	});
	section("Touch", {
		{{"Tap"}, "Use \xc2\xb7 tickle \xc2\xb7 slap"},
		{{"Hold"}, "Pick up \xc2\xb7 drop"},
		{{"Double-tap"}, "Talk to your Norns"},
		{{"Two fingers"}, "Look around \xc2\xb7 pinch to zoom"},
	});
	if (two) {
		ImGui::TableNextColumn();
	}
	section("Your Norns", {
		{{"Enter"}, "Talk to them"},
		{{"\xe2\x86\x91"}, "Say it again"},
		{{"1", "2", "3"}, "Give \xc2\xb7 Heal \xc2\xb7 Teach"},
		{{"Tab"}, "Next creature"},
		{{"F"}, "Follow"},
		{{"H"}, "Hatchery"},
		{{"V"}, "Creature's View"},
		{{"Space"}, "Pause"},
	});
	section("The world", {
		{{ctrl, "N"}, "New world"},
		{{ctrl, "O"}, "Load a world"},
		{{ctrl, "S"}, "Save"},
		{{"Drop"}, ".exp or .egg files: import"},
		{{"Alt", "Shift", "Right-drag"}, "A Norn out: export"},
	});
	if (two) {
		ImGui::EndTable();
	}
}

void draw_dialogs() {
	auto window_flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
	auto centre = [](float width = 0) { centre_window(width); };

	if (s_show_preferences) {
		centre();
		if (ImGui::Begin("Settings", &s_show_preferences, window_flags)) {
			ImGui::SeparatorText("Interface");
			static const char* layouts[] = {"Modern", "Classic"};
			int layout = s_options.classic_layout ? 1 : 0;
			if (UIStyle::segmented("##layout", layouts, 2, &layout)) {
				s_options.classic_layout = layout == 1;
			}
			ImGui::SetItemTooltip("Classic: the original's toolbar and status bar");
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
			if (ImGui::SliderFloat("Size", &s_options.ui_scale, 0.75f, 2.0f, "%.2f\xc3\x97")) {
				UIStyle::set_scale(s_options.ui_scale);
			}
			if (!s_options.classic_layout) {
				ImGui::Checkbox("Kits in one pane", &s_options.kits_in_panel);
				ImGui::SetItemTooltip("Off: a window for each kit");
				ImGui::Checkbox("Day and night", &s_options.day_and_night);
			} else {
				Kits::kit_look_option();
			}
			ImGui::SeparatorText("Playing");
			ImGui::Checkbox("Creature voices", &g_creature_speech_audible);
			ImGui::Checkbox("Sound in the background", &s_options.sound_in_background);
			ImGui::Checkbox("Follow the chosen Norn", &s_options.track_norn);
			ImGui::BeginDisabled(s_options.classic_layout && !Wolfling::active());
			ImGui::Checkbox("Keep food in the world", &s_options.food_for_population);
			ImGui::EndDisabled();
			ImGui::SetItemTooltip("Not in the original: a Norn with no food in sight gets some");
			bool autosave = autosave_enabled();
			if (ImGui::Checkbox("Autosave every 10 minutes", &autosave)) {
				set_autosave_enabled(autosave);
			}
			ImGui::Checkbox("Privileged mode", &g_engine_context.pointer->m_editing);
			ImGui::SetItemTooltip("The original's hidden cheat: the hand picks up creatures and Shift-drags anything");
			ImGui::Checkbox("Tips at start-up", &s_options.show_tips_on_startup);
			ImGui::Checkbox("Script errors", &s_options.show_script_errors);
			ImGui::SeparatorText("Camera");
			ImGui::Checkbox("Smooth scrolling", &g_engine_context.viewport->smooth_scrolling);
			ImGui::Checkbox("Wrap round", &g_engine_context.viewport->infinite_scroll);
			ImGui::SetItemTooltip("Scroll on past the world's ends, round to the other side");
			bool big = g_engine_context.viewport->scale > 1;
			if (ImGui::Checkbox("Magnify 2\xc3\x97", &big)) {
				g_engine_context.viewport->scale = big ? 2.0f : 1.0f;
			}
			// the modern layout has no menu bar: the Testing menu lives here
			if (!s_options.classic_layout && ImGui::CollapsingHeader("Testing")) {
				if (ImGui::BeginMenu("Testing tools")) {
					draw_testing_items();
					ImGui::EndMenu();
				}
			}
		}
		ImGui::End();
	}

	if (s_show_tip) {
		centre(420);
		if (ImGui::Begin("Tip of the Day", &s_show_tip, window_flags)) {
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
			ImGui::TextUnformatted(s_tips[s_tip_index % s_tips.size()].c_str());
			ImGui::PopTextWrapPos();
			ImGui::Separator();
			ImGui::Checkbox("Tips at start-up", &s_options.show_tips_on_startup);
			ImGui::SameLine();
			if (ImGui::Button("Next")) {
				s_tip_index = (s_tip_index + 1) % s_tips.size();
			}
			ImGui::SameLine();
			if (ImGui::Button("Close")) {
				s_show_tip = false;
				s_tip_index = (s_tip_index + 1) % s_tips.size();
			}
		}
		ImGui::End();
	}

	if (s_show_about) {
		centre();
		if (ImGui::Begin("About Creatures", &s_show_about, window_flags)) {
			ImGui::TextUnformatted("LibreC2e (opencreatures1)");
			ImGui::TextUnformatted("An open-source engine for Creatures (1996), built on openc2e and LibreCreatures.");
			ImGui::TextUnformatted("Creatures and its artwork belong to their copyright holders.");
			ImGui::TextUnformatted("This program plays your own copy of the game: none of it is included.");
		}
		ImGui::End();
	}

	if (s_show_help) {
		centre();
		if (ImGui::Begin("Help", &s_show_help, window_flags)) {
			draw_help();
		}
		ImGui::End();
	}

	if (s_show_add_place) {
		centre();
		static char name[32] = "";
		if (ImGui::Begin("Add a place", &s_show_add_place, window_flags)) {
			if (ImGui::IsWindowAppearing()) {
				ImGui::SetKeyboardFocusHere();
			}
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
			const bool entered = ImGui::InputTextWithHint("##place", "Name this view", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::BeginDisabled(!name[0]);
			if ((ImGui::Button("Add") || entered) && name[0]) {
				add_favourite_place(name);
				name[0] = '\0';
				s_show_add_place = false;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) {
				s_show_add_place = false;
			}
		}
		ImGui::End();
	}

	if (s_show_remove_place) {
		centre();
		static int chosen = -1;
		if (ImGui::Begin("Remove a place", &s_show_remove_place, window_flags)) {
			auto& places = g_engine_context.world->favorite_places;
			for (int i = 0; i < static_cast<int>(places.size()); ++i) {
				if (!places[static_cast<size_t>(i)].name.empty() && ImGui::Selectable(places[static_cast<size_t>(i)].name.c_str(), chosen == i)) {
					chosen = i;
				}
			}
			if (ImGui::Button("Remove") && chosen >= 0) {
				places[static_cast<size_t>(chosen)] = {};
				chosen = -1;
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) {
				s_show_remove_place = false;
			}
		}
		ImGui::End();
	}

	if (s_show_open) {
		const ImVec2 screen = ImGui::GetIO().DisplaySize;
		centre();
		ImGui::SetNextWindowSize(ImVec2(std::min(540.f, screen.x - 24), std::min(440.f, screen.y - 24)), ImGuiCond_Appearing);
		ImGui::SetNextWindowBgAlpha(0.98f);
		if (ImGui::Begin("Load a world", &s_show_open, ImGuiWindowFlags_NoCollapse)) {
			static std::string chosen;
			auto files = world_files();
			std::stable_sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
				std::error_code err;
				return fs::last_write_time(find_world(a), err) > fs::last_write_time(find_world(b), err);
			});
			// each world with a picture of what it is, its name, and when it was saved
			bool load_now = false;
			const float row = ImGui::GetTextLineHeight() * 2.2f;
			ImGui::BeginChild("worlds", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 6), ImGuiChildFlags_Borders);
			for (const auto& file : files) {
				auto path = find_world(file);
				std::error_code err;
				const auto time = fs::last_write_time(path, err);
				auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
					time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
				std::time_t stamp = std::chrono::system_clock::to_time_t(system_time);
				char date[64] = {};
				std::strftime(date, sizeof(date), "%b %d, %Y  %H:%M", std::localtime(&stamp));
				const auto lower = to_ascii_lowercase(file);
				const bool starter = lower == "eden.sfc", legacy = lower == "world.sfc", recovery = lower.find(".autosave.") != std::string::npos;
				const char* kind = starter ? "A fresh Eden" : legacy ? "The original's world" : recovery ? "Recovery save" : "Saved";
				const UIArt::Picture picture = starter ? UIArt::Picture::NewWorld : legacy ? UIArt::Picture::Open : recovery ? UIArt::Picture::Records : UIArt::Picture::Save;
				const ImVec2 p = ImGui::GetCursorScreenPos();
				if (ImGui::Selectable(("##" + file).c_str(), chosen == file, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, row))) {
					chosen = file;
					load_now = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
				}
				auto* d = ImGui::GetWindowDrawList();
				UIArt::picture(d, picture, ImVec2(p.x + row / 2, p.y + row / 2), row * 0.8f);
				d->AddText(ImVec2(p.x + row + 6, p.y + row * 0.08f), ImGui::GetColorU32(ImGuiCol_Text), fs::path(file).stem().string().c_str());
				d->AddText(ImVec2(p.x + row + 6, p.y + row * 0.52f), ImGui::GetColorU32(ImGuiCol_TextDisabled), fmt::format("{} \xc2\xb7 {}", kind, date).c_str());
			}
			ImGui::EndChild();
			ImGui::BeginDisabled(std::find(files.begin(), files.end(), chosen) == files.end());
			const bool load = ImGui::Button("Load", ImVec2(120, 0)) || load_now;
			ImGui::SetItemTooltip("Your world is kept as a recovery save first");
			if (load) {
				try {
					Kits::flush_records();
					auto checkpoint = fs::path(get_world_file_name()).stem().string() + ".autosave.before-load.sfc";
					if (checkpoint == chosen) checkpoint = fs::path(get_world_file_name()).stem().string() + ".autosave.before-load2.sfc";
					save_world(checkpoint);
				// Recoveries become a new manual slot; do not autosave under a nested name.
				std::string target;
				if (chosen.find(".autosave.") != std::string::npos) {
					target = chosen.substr(0, chosen.find(".autosave.")) + " Recovered.sfc";
					for (int n = 2; !find_world(target).empty(); ++n)
						target = chosen.substr(0, chosen.find(".autosave.")) + fmt::format(" Recovered {}.sfc", n);
				}
				request_world_load(chosen, target);
				s_show_open = false;
				} catch (const std::exception& e) { s_message = fmt::format("Couldn't keep a recovery copy: {}", e.what()); }
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) s_show_open = false;
		}
		ImGui::End();
	}

	if (s_show_new || s_show_save_as) {
		const bool creating = s_show_new;
		centre();
		ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.98f);
		static char name[128] = "My World";
		static std::string overwrite, error;
		bool open = true;
		if (ImGui::Begin(creating ? "New world" : "Save as", &open, window_flags)) {
			if (ImGui::IsWindowAppearing()) {
				ImGui::SetKeyboardFocusHere();
			}
			ImGui::SetNextItemWidth(-1);
			if (ImGui::InputTextWithHint("##worldname", "Its name", name, sizeof(name))) { overwrite.clear(); error.clear(); }
			std::string file;
			try { file = world_save_name(name); } catch (const std::exception& e) { error = e.what(); }
			bool exists = !file.empty() && !find_world(file).empty();
			if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
			if (exists) UIArt::glyph_text(UIArt::Glyph::Fear, IM_COL32(255, 196, 90, 255), "There's already a world with this name", ImGui::GetColorU32(ImGuiCol_TextDisabled));
			ImGui::BeginDisabled(file.empty());
			if (ImGui::Button(exists && overwrite == file ? "Yes, replace it" : creating ? "Create" : "Save", ImVec2(160, 0))) {
				if (exists && overwrite != file) overwrite = file;
				else try {
					Kits::flush_records();
					if (creating) {
						if (save_world_now()) { request_world_load("Eden.sfc", file); open = false; }
					} else {
						save_world(file);
						set_world_file_name(file); // change the active slot only after success
						show_message("Saved " + fs::path(file).stem().string());
						open = false;
					}
				} catch (const std::exception& e) { error = e.what(); }
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) open = false;
		}
		ImGui::End();
		if (!open) { s_show_new = s_show_save_as = false; overwrite.clear(); error.clear(); }
	}

}

// A blackboard in edit mode takes the keyboard (batch 4, Q19). Edit mode ends when the board
// goes off screen, or here also with Enter or Escape.
bool type_into_blackboard() {
	auto* obj = g_engine_context.objects->try_get(g_editing_blackboard);
	auto* board = obj ? obj->as_blackboard() : nullptr;
	if (!board || !board->editing) {
		g_editing_blackboard = {};
		return false;
	}
	auto camera = to_rectf(g_engine_context.viewport->get_main_camera_rect());
	auto b = board->get_bbox();
	if (b.right() < camera.x || b.x > camera.right() || b.bottom() < camera.y || b.y > camera.bottom()) {
		board->blackboard_disable_edit();
		return false;
	}
	auto& io = ImGui::GetIO();
	if (io.WantTextInput) {
		return false;
	}
	for (ImWchar c : io.InputQueueCharacters) {
		if (c < 128) {
			board->type_character(static_cast<char>(c));
		}
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
		board->type_backspace();
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		board->blackboard_disable_edit();
	}
	return true;
}

// Typing straight to the Norns, as in the original (batch 6, Q13): with the world view focused,
// Enter starts it; letters, digits, spaces and some punctuation go into the hand's words,
// shown in its thought cloud, up to 24 characters (batch 4); Backspace deletes; Enter says it,
// Escape cancels. Up brings back the last thing said (not in the original).

bool type_into_hand() {
	auto& io = ImGui::GetIO();
	if (!s_hand_typing) {
		if (s_touch_typing_requested || (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !io.KeyCtrl && !io.KeySuper)) {
			s_touch_typing_requested = false;
			s_hand_typing = true;
			s_hand_text.clear();
			sdl_want_keyboard(true);
			return true;
		}
		return false;
	}
	if (io.WantTextInput) {
		// a text field took the keyboard: stop typing to the Norns
		s_hand_typing = false;
		sdl_want_keyboard(false);
		s_hand_text.clear();
		return false;
	}
	for (ImWchar c : io.InputQueueCharacters) {
		const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '?' || c == '!' || c == '\'' || c == ',' || c == '.' || c == '-';
		if (allowed && s_hand_text.size() < 24) {
			s_hand_text += static_cast<char>(c);
		}
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !s_hand_text.empty()) {
		s_hand_text.pop_back();
	}
	if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && !s_last_sentence.empty()) {
		s_hand_text = s_last_sentence.substr(0, 24);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
		if (!s_hand_text.empty()) {
			fmt::print("INFO [Speech] the hand says {:?}\n", s_hand_text);
			say(s_hand_text);
		}
		s_hand_typing = false;
		sdl_want_keyboard(false);
		s_hand_text.clear();
	} else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		s_hand_typing = false;
		sdl_want_keyboard(false);
		s_hand_text.clear();
	}
	return true;
}

void handle_shortcuts() {
	auto& io = ImGui::GetIO();
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && (s_show_open || s_show_save_as || s_show_new)) {
		s_show_open = s_show_save_as = s_show_new = false;
		return;
	}
	if (type_into_blackboard() || io.WantTextInput) {
		return;
	}
	// while typing to the Norns, keys are words, not shortcuts
	if (type_into_hand()) {
		return;
	}
	const bool ctrl = io.KeyCtrl || io.KeySuper;
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
		s_show_new = true;
	}
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
		s_show_open = true;
	}
	if ((ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) || ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
		if (io.KeyShift) s_show_save_as = true;
		else save_world_now();
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) s_show_open = true;
	if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
		s_show_help = true;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
		s_show_debug_console = !s_show_debug_console;
	}
	if (ctrl) {
		return;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
		s_paused = !s_paused;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
		next_creature(io.KeyShift ? -1 : 1);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
		s_options.track_norn = !s_options.track_norn;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
		s_options.creatures_view = !s_options.creatures_view;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
		Kits::toggle("Hatchery");
	}
	// the modern bar's quick slots: Give, Heal, Teach
	if (!s_options.classic_layout) {
		const Tray trays[] = {Tray::Give, Tray::Heal, Tray::Teach};
		const ImGuiKey keys[] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3};
		for (size_t i = 0; i < 3; ++i) {
			if (ImGui::IsKeyPressed(keys[i], false)) {
				toggle_tray(trays[i], s_quick_x[i]);
			}
		}
	}
}

} // namespace

float modern_bar_scale() {
	return bar_scale();
}

bool begin_bar_area(const char* name, ImVec2 min, ImVec2 max) {
	return begin_hit_area(name, min, max);
}

Options& options() {
	return s_options;
}

void load_options() {
	// touch screens: a double tap on the world starts typing to the Norns, with the keyboard
	sdl_set_touch_double_tap_handler([](float, float) { s_touch_typing_requested = true; });
	auto path = options_path();
	std::ifstream in(path);
	std::string line;
	while (std::getline(in, line)) {
		auto eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string key = line.substr(0, eq);
		bool value = line.substr(eq + 1) == "1";
		if (key == "toolbar") s_options.show_toolbar = value;
		if (key == "status_bar") s_options.show_status_bar = value;
		if (key == "track_norn") s_options.track_norn = value;
		if (key == "editing") s_options.pick_up_creatures = value;
		if (key == "show_tips") s_options.show_tips_on_startup = value;
		if (key == "script_errors") s_options.show_script_errors = value;
		if (key == "speech") g_creature_speech_audible = value;
		if (key == "smooth_scrolling") g_engine_context.viewport->smooth_scrolling = value;
		if (key == "classic_layout") s_options.classic_layout = value;
		if (key == "food_for_population") s_options.food_for_population = value;
		if (key == "sound_in_background") s_options.sound_in_background = value;
		if (key == "day_and_night") s_options.day_and_night = value;
		if (key == "creatures_view") s_options.creatures_view = value;
		if (key == "kits_in_panel") s_options.kits_in_panel = value;
		if (key == "ui_scale") s_options.ui_scale = std::strtof(line.substr(eq + 1).c_str(), nullptr);
	}
	g_engine_context.pointer->m_editing = s_options.pick_up_creatures;
}

void save_options() {
	auto path = options_path();
	if (path.empty()) {
		return;
	}
	s_options.pick_up_creatures = g_engine_context.pointer->m_editing;
	std::ofstream out(path);
	out << "toolbar=" << s_options.show_toolbar << "\n";
	out << "status_bar=" << s_options.show_status_bar << "\n";
	out << "track_norn=" << s_options.track_norn << "\n";
	out << "editing=" << s_options.pick_up_creatures << "\n";
	out << "show_tips=" << s_options.show_tips_on_startup << "\n";
	out << "script_errors=" << s_options.show_script_errors << "\n";
	out << "speech=" << g_creature_speech_audible << "\n";
	out << "smooth_scrolling=" << g_engine_context.viewport->smooth_scrolling << "\n";
	out << "classic_layout=" << s_options.classic_layout << "\n";
	out << "food_for_population=" << s_options.food_for_population << "\n";
	out << "sound_in_background=" << s_options.sound_in_background << "\n";
	out << "day_and_night=" << s_options.day_and_night << "\n";
	out << "creatures_view=" << s_options.creatures_view << "\n";
	out << "ui_scale=" << s_options.ui_scale << "\n";
	out << "kits_in_panel=" << s_options.kits_in_panel << "\n";
}

// Touch screens: a finger dragged up or down over something that scrolls (a tray, a pane, a
// list) scrolls it, as on a phone; ImGui itself scrolls only with the wheel and scrollbars,
// which the interface hides. The drag lets go of the button it started on, so lifting the
// finger doesn't press it. A mouse is left as it is.
void touch_scrolling() {
	ImGuiContext& g = *ImGui::GetCurrentContext();
	static ImGuiWindow* s_scrolling = nullptr;
	if (!g.IO.MouseDown[0] || g.IO.MouseSource != ImGuiMouseSource_TouchScreen) {
		s_scrolling = nullptr;
		return;
	}
	if (!s_scrolling) {
		// a drag mostly up or down, far enough not to be a shaky tap
		const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0);
		if (std::abs(drag.y) < 12 || std::abs(drag.y) < std::abs(drag.x) * 1.5f) {
			return;
		}
		// the innermost window under the finger that scrolls
		for (ImGuiWindow* w = g.HoveredWindow; w; w = w->ParentWindow) {
			if (w->ScrollMax.y > 0 && !(w->Flags & ImGuiWindowFlags_NoScrollWithMouse)) {
				s_scrolling = w;
				break;
			}
			if (!(w->Flags & ImGuiWindowFlags_ChildWindow)) {
				break;
			}
		}
		if (!s_scrolling) {
			return;
		}
		ImGui::ClearActiveID();
		ImGui::SetScrollY(s_scrolling, s_scrolling->Scroll.y - drag.y);
		return;
	}
	ImGui::SetScrollY(s_scrolling, s_scrolling->Scroll.y - g.IO.MouseDelta.y);
}

void draw() {
	touch_scrolling();
	static std::string title;
	const auto current_title = "Creatures 1 — " + fs::path(get_world_file_name()).stem().string();
	if (title != current_title) {
		if (auto* backend = dynamic_cast<SDLBackend*>(get_backend()); backend && backend->window)
			SDL_SetWindowTitle(backend->window, current_title.c_str());
		title = current_title;
	}
	if (auto error = take_autosave_error(); !error.empty()) show_message(error);
	FileDialogs::poll();
	handle_dropped_files();
	handle_drag_out();
	// Cmd+, (Ctrl+, on Windows and Linux) opens the preferences
	if (ImGui::IsKeyPressed(ImGuiKey_Comma, false) && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper)) {
		s_show_preferences = true;
	}
	UIStyle::set_classic(s_options.classic_layout);
	// While paused the normal pointer shows everywhere, as in the original (batch 6, Q10), so the
	// menus, dialogs and kits are easy to use; the hand still follows it for placing objects.
	// The backend owns the pointer's visibility; this only says when it's wanted everywhere.
	sdl_set_pointer_always_visible(paused());
	// a click on the world while a wolfling run has locked the hand says why nothing happens
	if (g_engine_context.pointer->m_locked && !ImGui::GetIO().WantCaptureMouse && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
		static double last = -100;
		if (ImGui::GetTime() - last > 8) {
			last = ImGui::GetTime();
			const std::string why = "The hand is resting: this world has a wolfling run with strict or relaxed rules. End the run, or choose feral rules, to use the hand.";
			if (s_options.classic_layout) {
				if (s_message.empty()) s_message = why;
			} else {
				toast(why);
			}
		}
	}
	g_engine_context.pointer->m_locked = Wolfling::hand_locked();
	// Changes to world scripts as they're queued. Modern mode has no population limit: the
	// world scripts' own limits (12 Norns for an egg to hatch, 8 for the incubator to take
	// eggs; batch 6, Q4) are lifted, while classic mode keeps them.
	if (!g_engine_context.macros->script_filter) {
		g_engine_context.macros->script_filter = [](uint8_t f, uint8_t g, uint8_t s, ScriptNumber event, std::string& script) {
			// In both modes, a food's eat script (family 2, event 17) no longer starts with
			// "drop". With empty hands that drop is a self-disappointment, which wipes the
			// Norn's memory of other Norns; one is then noticed as new and takes its attention
			// off the food, so in company most meals failed and Norns starved (batch 8). A
			// pickup already makes it let go of anything it was carrying.
			if (f == 2 && (g == 6 || g == 4) && event == SCRIPT_EXTRA_ACTIVATE1 && script.rfind("drop,", 0) == 0) {
				script.erase(0, 5);
			}
			if (s_options.classic_layout || f != 2 && f != 3) {
				return;
			}
			auto replace = [&script](const std::string& from, const std::string& to) {
				for (size_t at = script.find(from); at != std::string::npos; at = script.find(from, at + to.size())) {
					script.replace(at, from.size(), to);
				}
			};
			if (f == 2 && g == 5 && s == 2) {
				replace("totl 4 1 0 ge 12", "totl 4 1 0 ge 30000");
			} else if (f == 3 && g == 1 && s == 3) {
				replace("var0 ge 8", "var0 ge 30000");
				replace("var0 lt 8", "var0 lt 30000");
			}
		};
	}
	// the window's title: "Creatures", or "Creatures - name" with a creature selected, as the
	// original's (batch 6, Q10)
	{
		static std::string shown_title;
		auto* c = selected_creature();
		const std::string title = c ? "Creatures - " + creature_label(*c) : "Creatures";
		if (title != shown_title) {
			shown_title = title;
			int count = 0;
			SDL_Window** windows = SDL_GetWindows(&count);
			if (windows && count > 0) {
				SDL_SetWindowTitle(windows[0], title.c_str());
			}
			SDL_free(windows);
		}
	}

	// debugging: OPENCREATURES1_TEST_PHOTO takes a photo of the selected creature after 3 s
	// of play and saves the album's latest photo as test-photo.bmp
	if (std::getenv("OPENCREATURES1_TEST_PHOTO")) {
		static int stage = 0;
		static double at = ImGui::GetTime() + 3;
		if (auto* c = selected_creature(); c && ImGui::GetTime() > at) {
			if (stage == 0) {
				PhotoAlbum::take_photo(*c);
				stage = 1;
			} else if (stage == 1) {
				auto photos = PhotoAlbum::load(c->moniker);
				if (!photos.empty()) {
					PhotoAlbum::save_bmp(photos.back(), "test-photo.bmp");
					stage = 2;
				}
			}
		}
	}
	// debugging: OPENCREATURES1_TEST_EXPORT exports the selected creature after 3 s (keeping it)
	// and imports the named .exp file
	if (const char* imported = std::getenv("OPENCREATURES1_TEST_EXPORT")) {
		static bool done = false;
		static double at = ImGui::GetTime() + 3;
		if (auto* c = selected_creature(); c && !done && ImGui::GetTime() > at) {
			done = true;
			try {
				export_creature(*c, false);
				auto [x, y] = view_centre();
				auto* in = import_creature(imported, x, y);
				fmt::print("TEST exported {} and imported {}\n", c->moniker, in ? in->moniker : "nothing");
			} catch (std::exception& e) {
				fmt::print("TEST export/import failed: {}\n", e.what());
			}
		}
	}
	// the modern layout has no menu bar: its menus' work is on the bar, the world plaque and
	// in Settings
	RecordingUI::testing();
	float top = 0;
	if (RecordingUI::playing_back()) {
		// a recording playing back: only its bar and windows (and the classic menu bar)
		if (s_options.classic_layout) {
			draw_menu_bar();
			top = ImGui::GetFrameHeight();
		}
		g_engine_context.viewport->set_margin_top(top);
		g_engine_context.viewport->set_margin_bottom(RecordingUI::draw(top));
		HdArt::set_enabled(!s_options.classic_layout);
		g_engine_context.pointer->m_over_interface = true;
		draw_dialogs();
		draw_message_boxes();
		return;
	}
	RecordingUI::draw_recording_light();
	float plaque_right = 0;
	if (s_options.classic_layout) {
		draw_menu_bar();
		top = ImGui::GetFrameHeight();
	} else {
		plaque_right = draw_world_plaque();
	}
	watch_for_news();
	// the thought cloud shows what's being typed straight to the Norns, or into the classic speech box
	update_hand_thought(s_hand_typing ? (s_hand_text.empty() ? std::string("_") : s_hand_text + "_") : std::string(s_sentence));
	float bottom = 0;
	if (s_options.classic_layout) {
		const float toolbar = draw_toolbar(top);
		bottom = draw_status_bar();
		g_engine_context.viewport->set_margin_top(top + toolbar);
	} else {
		bottom = draw_modern_bar();
		g_engine_context.viewport->set_margin_top(top);
		draw_clock(top, plaque_right);
	}
    draw_toasts(top);
    draw_error_details();
	draw_porthole(bottom);
	g_engine_context.viewport->set_margin_bottom(bottom);

	g_creature_angry_face = !s_options.classic_layout;
	g_cob_unlimited = !s_options.classic_layout;
	// the player's higher-resolution sprites, where there are some, in the modern layout
	HdArt::set_enabled(!s_options.classic_layout);
#ifdef OPENC1_LOCAL_FEATURES
	local_frame(); // local additions (src/opencreatures1/local/), not published
#endif
	// the modern layout zooms with Shift + the wheel too, and out as well as in; the classic
	// layout keeps the original's size or larger
	g_engine_context.viewport->shift_wheel_zooms = !s_options.classic_layout;
	g_engine_context.viewport->min_scale = s_options.classic_layout ? 1.0f : 0.5f;
	if (g_engine_context.viewport->scale < g_engine_context.viewport->min_scale) {
		g_engine_context.viewport->scale = g_engine_context.viewport->min_scale;
		g_engine_context.viewport->update();
	}
	g_engine_context.pointer->m_shift_down = ImGui::GetIO().KeyShift;
	g_engine_context.pointer->m_over_interface = ImGui::GetIO().WantCaptureMouse;

	// below the clock in the modern layout
	// below the clock, and above the medallions on the modern bar
	Kits::set_panel(s_options.kits_in_panel, s_options.classic_layout ? top : top + ImGui::GetFrameHeight() * 2.2f, s_options.classic_layout ? bottom : ImGui::GetIO().DisplaySize.y - s_tray_bottom);
	draw_trays();
	Kits::draw_windows();
	Wolfling::draw_windows();
	Family::draw_windows();
	draw_crash_windows();
	draw_dialogs();
	draw_message_boxes();
	draw_world_markers();
	handle_shortcuts();
}

void set_paused(bool paused) { s_paused = paused; }

bool paused() {
	return s_paused || RecordingUI::playing_back() || s_native_drag_active || s_show_open || s_show_new || s_show_save_as;
}

// Alerts (both modes): a Norn's life force falling to 20 %, an egg being laid, a Norn dying.
// The original's Observation Kit warned of births, pregnancies and low life force; here the
// game does it whether or not the kit is open. Modern mode shows a notification, classic a
// message box.
struct AlertState {
	std::set<std::string> low; // monikers already warned about, until they recover
	std::set<std::string> dead;
	std::vector<ObjectHandle> eggs;
	bool primed = false; // the world's state when it loads isn't news
};
AlertState s_alerts;

void alert(const std::string& text, ObjectHandle about) {
	fmt::print("INFO [Alert] {}\n", text);
	if (s_options.classic_layout) {
		if (s_message.empty()) {
			s_message = text;
		}
	} else {
		toast(text, about);
	}
}

void check_alerts() {
	std::vector<ObjectHandle> eggs;
	for (auto* obj : *g_engine_context.objects) {
		if (obj->family == 2 && obj->genus == 5 && obj->species == 2) {
			eggs.push_back(obj->uid);
			// a natural egg (obv1 0; the Hatchery's have 1 or 2) that wasn't there before
			const bool known = std::find(s_alerts.eggs.begin(), s_alerts.eggs.end(), obj->uid) != s_alerts.eggs.end();
			if (s_alerts.primed && !known && obj->obv1 == 0) {
				alert("A Norn has laid an egg!", obj->uid);
			}
		}
		auto* c = obj->as_creature();
		if (!c || c->genus != 1) {
			continue;
		}
		if (c->dead) {
			if (s_alerts.primed && !s_alerts.dead.count(c->moniker)) {
				alert(fmt::format("{} has died.", creature_label(*c)), c->uid);
			}
			s_alerts.dead.insert(c->moniker);
			continue;
		}
		const int32_t health = creature_health_percent(*c);
		if (health <= 20 && !s_alerts.low.count(c->moniker)) {
			s_alerts.low.insert(c->moniker);
			if (s_alerts.primed) {
				alert(fmt::format("{}'s life force is down to {}%.", creature_label(*c), health), c->uid);
			}
		} else if (health >= 30) {
			s_alerts.low.erase(c->moniker);
		}
	}
	s_alerts.eggs = std::move(eggs);
	s_alerts.primed = true;
}

void tick() {
	track_norn_tick();
	creatures_view_tick();
	Wolfling::tick();
	Family::tick();
	if (g_engine_context.world->tick % 100 == 0) {
		check_alerts();
	}
	// modern and wolfling-run feature only: classic play keeps the original's food levels
	if (s_options.food_for_population && (!s_options.classic_layout || Wolfling::active()) && g_engine_context.world->tick % 600 == 0) {
		Wolfling::keep_food_for_population();
	}
}

int32_t game_speed() {
	return s_speed;
}

void set_game_speed(int32_t speed) {
	s_speed = speed >= 99 ? 99 : std::clamp(speed, 1, 4);
}

void render_world_views(RenderSystem& system) {
	UIImages::creatures_view().render(system);
	UIImages::take_requested_photo(system);
}

bool quit_requested() {
	return s_quit;
}

void request_world_load(const std::string& file, const std::string& save_as) {
	s_world_to_load = file;
	s_new_world_name = save_as;
}

std::string take_world_to_load() {
	std::string file = s_world_to_load;
	s_world_to_load.clear();
	return file;
}

void world_load_failed() { s_new_world_name.clear(); }

bool world_loaded() {
	// a loaded world's eggs, deaths and weak Norns aren't news
	s_alerts = AlertState{};
	s_news_primed = false;
	s_known_events.clear();
	s_announced_deaths.clear();
	s_toasts.clear();
	const bool new_world = !s_new_world_name.empty();
	if (new_world) {
		set_world_file_name(s_new_world_name);
		s_new_world_name.clear();
	}
	s_health = -1;
	return new_world;
}

void show_message(const std::string& text) {
	if (s_options.classic_layout) {
		s_message = text;
	} else {
		toast(text);
	}
}

void set_typed_text_for_testing(const std::string& text) {
	std::snprintf(s_sentence, sizeof(s_sentence), "%s", text.c_str());
}

void show_startup_tip() {
	if (s_options.show_tips_on_startup) {
		open_tip();
	}
}

} // namespace GameUI
