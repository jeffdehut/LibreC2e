#include "UIText.h"

#include <algorithm>
#include <cmath>
#include <imgui_internal.h>

namespace UIText {

namespace {

bool is_digit(char c) {
	return c >= '0' && c <= '9';
}

// Lays text out a run at a time, each digit centred in a cell as wide as the widest digit and
// everything else as the font has it, calling place(x, begin, end) for each run; returns the
// width.
template <typename Place>
float steady_layout(const char* text, Place&& place) {
	float digit = 0;
	for (char d = '0'; d <= '9'; ++d) {
		digit = std::max(digit, ImGui::CalcTextSize(&d, &d + 1).x);
	}
	float x = 0;
	for (const char* p = text; *p;) {
		const char* end = p + 1;
		if (is_digit(*p)) {
			place(x + (digit - ImGui::CalcTextSize(p, end).x) / 2, p, end);
			x += digit;
		} else {
			while (*end && !is_digit(*end)) {
				++end;
			}
			place(x, p, end);
			x += ImGui::CalcTextSize(p, end).x;
		}
		p = end;
	}
	return x;
}

void draw_steady(ImDrawList* draw, ImVec2 at, const char* text, ImU32 colour) {
	steady_layout(text, [&](float x, const char* begin, const char* end) {
		draw->AddText(ImVec2(std::floor(at.x + x), at.y), colour, begin, end);
	});
}

} // namespace

float steady_width(const char* text) {
	return steady_layout(text, [](float, const char*, const char*) {});
}

void steady_text(const char* text, const char* reserve, ImU32 colour, float align) {
	const float width = steady_width(text);
	const float space = std::max(width, steady_width(reserve));
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(space, ImGui::GetTextLineHeight()));
	draw_steady(ImGui::GetWindowDrawList(), ImVec2(at.x + (space - width) * align, at.y), text, colour);
}

bool steady_small_button(const char* id, const char* text, const char* reserve) {
	const ImGuiStyle& style = ImGui::GetStyle();
	const float width = steady_width(text);
	const ImVec2 size(std::max(width, steady_width(reserve)) + style.FramePadding.x * 2, ImGui::GetTextLineHeight());
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(id, size);
	const ImGuiCol fill = ImGui::IsItemActive() ? ImGuiCol_ButtonActive : ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button;
	ImGui::RenderFrame(at, ImVec2(at.x + size.x, at.y + size.y), ImGui::GetColorU32(fill), true, style.FrameRounding);
	draw_steady(ImGui::GetWindowDrawList(), ImVec2(at.x + (size.x - width) / 2, at.y), text, ImGui::GetColorU32(ImGuiCol_Text));
	return pressed;
}

} // namespace UIText
