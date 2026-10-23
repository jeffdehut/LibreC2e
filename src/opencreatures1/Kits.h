#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "UIImages.h"

struct Creature;

// The kits (Clean Specs/BATCH3-ANSWERS.md, Q11-Q13). In the original each kit is a separate
// program that talks to the game; here they're windows inside the game. Each works on the
// selected creature, its "subject".
namespace Kits {

// items for the Tools menu and buttons for the toolbar, one per kit
void draw_menu_items();
void draw_toolbar_buttons();

// the open kit windows
void draw_windows();
// Classic layout: a checkbox for the kits' look, LibreCreatures' pages (the default) or the
// 1996 pages with their covers
void kit_look_option();
// Modern layout: show the kits as panes (below) instead of separate windows. top and bottom
// are the interface's margins above and below the view.
void set_panel(bool tabbed, float top, float bottom);

// The modern layout's panes (not in the original): the kits' pages without their covers,
// gathered by what they're about, each fitting its window without scrolling.
//   Norn: the Owner's, Health and Breeder's kits; Lab: the Science Kit; Objects: the
//   Hatchery, the Object Injector and the herb shops; Records: Observation, the Graveyard,
//   Score and Emergency.
enum class Pane { None, Norn, Lab, Objects, Records };
// opens a pane at one of its sections (0 is the first); asked for again, it closes
void open_pane(Pane pane, int section = 0);
Pane current_pane();
const char* pane_name(Pane pane);

// Things the bottom bar's trays put into the world.
struct QuickItem {
	std::string key; // what add_item needs
	std::string name;
	std::string description;
	const UIImages::Bitmap* picture = nullptr;
	int remaining = -1; // -1 for unlimited
};
// the Object Injector's add-on objects (with objects_only, not the removers)
std::vector<QuickItem> injector_items(bool objects_only = false);
// the Doctor's page's herbs and food (and the Breeder's Kit's with breeding)
std::vector<QuickItem> shop_items(bool breeding = false);
// puts one into the world (most go into the hand, as the kits' ADD does); false if it
// couldn't (the reason is shown in the kits)
bool add_item(const QuickItem& item);
// the Science Kit's injections (injections.str) and giving one to a creature
const std::vector<std::string>& injection_names();
void inject(Creature& creature, size_t mixture, int dose = 128);

// opens or closes a kit by its toolbar name ("Hatchery", "Owner", ...), and whether it's open
void toggle(const std::string& name);
bool is_open(const std::string& name);

// names from the game's own files: chemicals.str and decision.str
std::string chemical_name(size_t chemical);
std::string decision_name(int32_t decision);

// debugging: opens the kits whose toolbar names are in the list ("all" for every kit)
void open_for_testing(const std::string& names);

// forget anything kept about the current world (call after loading another world)
void world_changed();
// Flush death snapshots before replacing a world or shutting down.
void flush_records();

} // namespace Kits
