#pragma once

#include <imgui.h>

// Art for the modern interface, drawn in code in the spirit of Albia's hand-made look:
// carved wood, brass, glass and leaves. None of it comes from the game's files (the faces,
// object pictures and the world view it frames do, at runtime, from the player's copy).
namespace UIArt {

// warm brass and wood colours, shared by the bar, the trays and the panes
constexpr ImU32 BRASS_DARK = IM_COL32(92, 64, 24, 255);
constexpr ImU32 BRASS = IM_COL32(186, 142, 64, 255);
constexpr ImU32 BRASS_LIGHT = IM_COL32(242, 212, 136, 255);
constexpr ImU32 WOOD_DARK = IM_COL32(44, 26, 14, 255);
constexpr ImU32 INK = IM_COL32(250, 238, 212, 255); // text on wood
constexpr ImU32 INK_SOFT = IM_COL32(222, 196, 150, 220);

// a carved wooden panel: a grain texture, bevelled edges and rounded corners; darken (0-1)
// sinks it, for an inset
void wood_panel(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float darken = 0);
// a brass band, for trim and name plates
void brass_plate(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding);
// a brass ring between two radii, lit from the top left
void brass_ring(ImDrawList* draw, ImVec2 centre, float outer, float inner);
void rivet(ImDrawList* draw, ImVec2 centre, float radius);
// dark glass, and its shine on top of whatever is drawn behind the glass
void glass(ImDrawList* draw, ImVec2 centre, float radius);
void glass_shine(ImDrawList* draw, ImVec2 centre, float radius);
// a meter round a medallion: an arc from the bottom, fraction 0-1 of the way round
void ring_meter(ImDrawList* draw, ImVec2 centre, float radius, float thickness, float fraction, ImU32 colour);
// a leaf from base, length long, pointing at angle (radians, 0 right, y down)
void leaf(ImDrawList* draw, ImVec2 base, float length, float angle, ImU32 colour);
// text with a soft dark edge so it reads on wood or over the world
void text(ImDrawList* draw, ImVec2 pos, float size, ImU32 colour, const char* text, bool shadow = true);

enum class Picture {
	Give, // a carrot and a wedge of cheese
	Heal, // a medicine bottle with a sprig of herbs
	Teach, // a speech scroll with letters
	Norn, // a little Norn head
	Lab, // a flask
	Objects, // a crate
	Records, // a book
	Follow,
	Egg,
	Places,
	Voices,
	Quiet,
	More,
	Tickle, // a hand with a heart
	Slap, // a hand with a cross
	NewWorld, // a seedling
	Open, // an open book
	Save, // a floppy disk
	SaveAs, // a floppy disk and a quill
	Import, // an egg with an arrow in
	Export, // an egg with an arrow out
	Settings, // a cog
	Help, // a question mark
	Quit, // a door
	Tree, // a family tree
	Paw, // a paw print
	Record, // a red recording light in a ring
	// a player's controls
	ToStart, // a bar and two triangles pointing left
	StepBack, // a triangle pointing left and a bar
	Backwards, // two triangles pointing left
	Play, // a triangle pointing right
	Pause, // two bars
	StepOn, // a bar and a triangle pointing right
	ToEnd, // two triangles pointing right and a bar
};
// painted pictures for the bar's buttons, fitting a box size across
void picture(ImDrawList* draw, Picture picture, ImVec2 centre, float size, bool lit = false);
// the player's control pictures (ToStart to ToEnd) in one colour, for plainer buttons
void controls(ImDrawList* draw, Picture picture, ImVec2 centre, float size, ImU32 colour);

// A round brass button at a screen point: an invisible button for the clicks and the art
// in the given draw list (the bar draws into the background list, behind every window).
// "on" shows it pressed in and glowing. Returns true when clicked.
bool round_button(ImDrawList* draw, const char* id, ImVec2 centre, float radius, Picture picture, const char* tooltip, bool on = false, bool enabled = true);

// A value easing towards target (0-1) over about a tenth of a second, kept per key: for hover
// and open/close animations.
float ease(unsigned key, float target, float speed = 14);

// The one shadow every piece of the modern interface casts: soft, faint and a little below it.
void shadow(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding);
void shadow_circle(ImDrawList* draw, ImVec2 centre, float radius);

// a wooden frame for a window: call first thing after Begin, with the window's rectangle
// paper: the inside a page of warm paper (the kits' panes) rather than dark
void window_frame(ImDrawList* draw, ImVec2 min, ImVec2 max, float border, const char* title = nullptr, bool paper = false);

} // namespace UIArt
