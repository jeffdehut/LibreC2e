#pragma once

#include <functional>
#include <string>

// Classic mode's kit windows. In the original each kit is a separate program with its own
// window, which can sit in front of or behind the game's (batch 6, Q11). Here each open kit
// gets a system window of its own, with its own renderer and interface context, drawn once a
// frame alongside the main window.
namespace KitWindows {

// Shows a kit's window this frame, creating it if needed. body draws the kit's contents.
// Clearing *open (the window's close button) closes it. A fixed size (the Hatchery's
// picture) makes the window that size and not resizable; 0 x 0 is a resizable window.
void show(int id, const std::string& title, bool* open, bool on_top, int fixed_width, int fixed_height, const std::function<void()>& body);

// Closes the windows that weren't shown since the last call. Call once a frame, after the
// kits have been shown.
void end_frame();

// Closes every kit window (leaving classic mode, or quitting).
void close_all();

} // namespace KitWindows
