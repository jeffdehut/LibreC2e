#pragma once

#include <imgui.h>

// Text with numbers that change, such as a clock or a score, laid out so it holds still: each
// digit takes the widest digit's width, so the text doesn't wobble as the digits change, and
// the space kept is at least a reserve's (laid out the same way, such as "00:00:00"), so
// nothing beside it moves as the numbers grow. Only ImGui is needed, so the core's windows
// can use it too.
namespace UIText {

// The text, placed in its space by align: 0 left, 0.5 centre, 1 right.
void steady_text(const char* text, const char* reserve, ImU32 colour, float align = 0);
// The width steady_text gives text.
float steady_width(const char* text);
// A small button (as ImGui::SmallButton) with a steady label, kept as wide as reserve's.
bool steady_small_button(const char* id, const char* text, const char* reserve);

} // namespace UIText
