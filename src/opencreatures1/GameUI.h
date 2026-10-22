#pragma once

#if defined(__APPLE__) && defined(__has_include)
#if __has_include(<TargetConditionals.h>)
#include <TargetConditionals.h>
#endif
#endif

#include <stdint.h>
#include <string>

class RenderSystem;

// The player's interface, following Clean Specs/BATCH3-ANSWERS.md (Q1-Q10): the menu bar,
// toolbar and status bar, the camera options, favourite places, the selected creature,
// typing to creatures, the keyboard, tips and messages.
namespace GameUI {

// options the player can change; saved between sessions
struct Options {
	// keep enough food in the world for its population (not in the original)
	bool food_for_population = true;
	bool show_toolbar = true;
	bool show_status_bar = true;
	bool track_norn = false;
	bool creatures_view = false;
	bool pick_up_creatures = false; // the editing option (batch 2, Q12); off unless chosen
	bool show_tips_on_startup = true;
	bool show_script_errors = true;
	// the modern layout (a bottom panel with the selected Norn, the roster and the tools) or
	// the original's toolbar and status bar
	bool classic_layout = false;
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
	float ui_scale = 1.4f; // fingers need bigger targets
#else
	float ui_scale = 1.0f;
#endif
	// modern layout: the kits in one panel with tabs down its side, rather than windows
	bool kits_in_panel = true;
	bool sound_in_background = false; // the original goes quiet in the background
};
Options& options();
void load_options();
void save_options();

// Draws the interface for this frame. Call once per frame, before the world is drawn.
void draw();

// Runs once per game tick: Track Norn and Creature's View move with the game, not the frame
// rate.
void tick();

// Whether the game is paused (the toolbar's Pause / Resume).
bool paused();
void set_paused(bool paused);
// How many times faster than normal the game runs: 1, 2 or 4. The original has no speed
// setting; this is an addition.
int32_t game_speed();
void set_game_speed(int32_t speed);

// Draws views of the world the interface shows (Creature's View). Call while drawing the
// world, before the main view.
void render_world_views(RenderSystem&);

// Whether the player asked to quit.
bool quit_requested();

// A world the player asked to open, relative to the main directory, or empty. The main loop
// loads it at a safe point and then calls world_loaded().
std::string take_world_to_load();
// asks for a world to be loaded, then saved under save_as (or its own name if empty)
void request_world_load(const std::string& file, const std::string& save_as = "");
// Finalize the save name; return whether this was a newly created world.
bool world_loaded();
void world_load_failed();

// shows a message: a notification in the modern layout, a message box in the classic one
void show_message(const std::string& text);

// debugging: puts text in the speech box, as if the player had typed it
void set_typed_text_for_testing(const std::string& text);

// Shows the Tip of the Day box if the player wants it on startup.
void show_startup_tip();

} // namespace GameUI
