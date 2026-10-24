#pragma once

#include <filesystem>

class RenderSystem;

// Recording a world and playing recordings back (Recording.h), in both layouts. While one
// plays, the world is paused and out of sight: the main view shows the recording, which can
// be scrubbed, played backwards and forwards at any speed, and looked round; clicking a
// creature shows what it was doing, and its inspector shows its drives, chemicals and brain
// as they were. Nothing in a recording can be changed.
namespace RecordingUI {

bool playing_back();
// opens a recording to play (a message says why if it can't)
void open(const std::filesystem::path& file);
void close();
// starts recording the world, or stops
void toggle_recording();
// where the world's recordings go: its folder's Recordings folder
std::filesystem::path folder();

// While playing back, the interface: the bar along the bottom (kept clear of top, the
// classic menu bar), the creature's details and inspector. Returns the height the bar keeps
// at the bottom of the window.
float draw(float top);
// the recording's picture at the playing tick, for the main view
void render(RenderSystem& system);
// menu entries for recording and playing back (the classic World menu, the modern More menu)
void menu_items();
// a small red light in a corner of the view while recording
void draw_recording_light();
// debugging: OPENCREATURES1_TEST_RECORDING=<seconds>[,<playback tick fraction>] records the
// world for that long, then plays it back from that far in (default half), with the first
// creature's details and inspector open. Call once a frame.
void testing();

} // namespace RecordingUI
