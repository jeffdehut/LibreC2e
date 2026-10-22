#pragma once

#include <imgui.h>

// The interface's look, after Apple's design guidance: one clear system font (San Francisco
// on a Mac), translucent dark panels with soft shadows, Apple's grey text levels and a
// single accent colour (Albia's leaf green), capsule controls, continuous rounded corners,
// and simple vector icons. An adjustable interface scale.
namespace UIStyle {

// Loads the fonts and applies the theme, in the current interface context. Call once after
// the backend is set up; other windows' contexts (classic kit windows) pass false, since
// their renderer makes its own font texture.
void init(bool main_window = true);

// Classic mode's Windows 95 look, or the modern one. Reloads the fonts and theme.
void set_classic(bool classic);
bool classic();

// Applies a new interface scale (1 = normal).
void set_scale(float scale);
float scale();

// a larger font for names and titles (the normal font if it couldn't be loaded)
ImFont* title_font();

// Colours.
ImU32 accent();
ImU32 accent_soft();
ImU32 panel();
ImU32 text_dim();
ImU32 separator();
ImVec4 health_colour(float fraction);

// A soft shadow behind a rounded rectangle, drawn on the background so it sits under windows.
void shadow(ImVec2 min, ImVec2 max, float rounding);
// Gives the next window a shadow; call after it's ended with its position and size.
void window_shadow();

// A thin capsule meter for a 0-1 value, then an optional label.
void meter(const char* label, float fraction, ImVec4 colour, float width);

// Icons, drawn into a square.
enum class Icon {
	Pause,
	Play,
	Eye,
	Follow,
	Egg,
	Kits,
	Places,
	Teach,
	SpeakerOn,
	SpeakerOff,
	Say,
	History,
	Camera,
	MedKit,
	Paw,
	FamilyTree,
};
void draw_icon(ImDrawList* draw, Icon icon, ImVec2 min, float size, ImU32 colour);

// A capsule button with an icon and an optional label; "on" shows it selected.
bool icon_button(const char* id, Icon icon, const char* label, const char* tooltip, bool on = false);

// The modern toolbar's buttons: a rounded square with just an icon (the tooltip says what it
// does), so they stand apart from the round Norn faces; "on" shows it selected.
bool tool_button(const char* id, Icon icon, const char* tooltip, bool on = false);

// The privileged-mode toggle: a purple Norn plushie in a rounded square, the size of a tool
// button unless size is given; faded when off, glowing when on.
bool plushie_button(const char* id, const char* tooltip, bool on, float size = 0);

// A segmented control (e.g. 1x / 2x / 4x); returns true and sets selected when it changes.
bool segmented(const char* id, const char* const* labels, int count, int* selected);

} // namespace UIStyle
