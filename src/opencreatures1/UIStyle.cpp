#include "UIStyle.h"
#include "UIArt.h"

#include "sdlbackend/SDLBackend.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <imgui_internal.h>
#include <string>

#include <map>

namespace UIStyle {

namespace {

float s_scale = 1.0f;
ImGuiStyle s_base_style;
// each window's interface has its own fonts
struct Fonts {
	ImFont* regular = nullptr; // modern
	ImFont* title = nullptr; // modern
	ImFont* classic = nullptr;
};
std::map<ImGuiContext*, Fonts> s_fonts;
bool s_classic = false;

// Fonts are rasterised at twice their size and shown at half scale, so text stays sharp on
// high-density displays.
constexpr float FONT_OVERSAMPLE = 2.0f;
constexpr float FONT_SIZE = 14.5f;
constexpr float TITLE_SIZE = 19.0f;

const char* find_font(bool title) {
	// the platform's own interface font; the game ships none
	static const char* text[] = {
		"/System/Library/Fonts/SFNS.ttf",
		"/System/Library/Fonts/HelveticaNeue.ttc",
		"C:\\Windows\\Fonts\\SegUIVar.ttf",
		"C:\\Windows\\Fonts\\segoeui.ttf",
		"/usr/share/fonts/truetype/inter/Inter-Regular.ttf",
		"/usr/share/fonts/truetype/cantarell/Cantarell-Regular.otf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/TTF/DejaVuSans.ttf",
		"/usr/share/fonts/noto/NotoSans-Regular.ttf",
	};
	static const char* titles[] = {
		"/System/Library/Fonts/SFNSRounded.ttf",
		"/System/Library/Fonts/SFNS.ttf",
		"C:\\Windows\\Fonts\\seguisb.ttf",
		"C:\\Windows\\Fonts\\segoeui.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
		"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
	};
	const char* const* list = title ? titles : text;
	const size_t count = title ? std::size(titles) : std::size(text);
	for (size_t i = 0; i < count; ++i) {
		std::error_code err;
		if (std::filesystem::exists(list[i], err)) {
			return list[i];
		}
	}
	return nullptr;
}

ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
	return ImVec4(static_cast<float>(r) / 255.f, static_cast<float>(g) / 255.f, static_cast<float>(b) / 255.f, a);
}

// Classic mode: Windows 95. Grey bevelled controls, square corners, black text, white edit
// fields, and a small bitmap font (ImGui's own ProggyClean, standing in for MS Sans Serif).
void apply_classic_theme() {
	ImGuiStyle& style = ImGui::GetStyle();
	style = ImGuiStyle();
	style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = 0;
	style.ScrollbarRounding = style.GrabRounding = style.TabRounding = 0;
	style.WindowBorderSize = 1;
	style.FrameBorderSize = 1;
	style.PopupBorderSize = 1;
	style.TabBorderSize = 1;
	style.WindowPadding = ImVec2(8, 8);
	style.FramePadding = ImVec2(6, 3);
	style.ItemSpacing = ImVec2(6, 5);
	style.ScrollbarSize = 16;
	style.GrabMinSize = 10;
	style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
	ImVec4* c = style.Colors;
	const ImVec4 face = rgb(192, 192, 192), shadow = rgb(128, 128, 128), white = rgb(255, 255, 255), black = rgb(0, 0, 0);
	const ImVec4 navy = rgb(0, 0, 128);
	c[ImGuiCol_Text] = black;
	c[ImGuiCol_TextDisabled] = shadow;
	c[ImGuiCol_WindowBg] = face;
	c[ImGuiCol_ChildBg] = face;
	c[ImGuiCol_PopupBg] = face;
	c[ImGuiCol_Border] = shadow;
	c[ImGuiCol_BorderShadow] = white;
	c[ImGuiCol_FrameBg] = white;
	c[ImGuiCol_FrameBgHovered] = white;
	c[ImGuiCol_FrameBgActive] = white;
	// title bars: the light end of the Windows 98 caption, so the black title text reads
	c[ImGuiCol_TitleBg] = shadow;
	c[ImGuiCol_TitleBgActive] = rgb(166, 202, 240);
	c[ImGuiCol_TitleBgCollapsed] = shadow;
	c[ImGuiCol_MenuBarBg] = face;
	c[ImGuiCol_ScrollbarBg] = rgb(224, 224, 224);
	c[ImGuiCol_ScrollbarGrab] = face;
	c[ImGuiCol_ScrollbarGrabHovered] = face;
	c[ImGuiCol_ScrollbarGrabActive] = shadow;
	c[ImGuiCol_CheckMark] = black;
	c[ImGuiCol_SliderGrab] = face;
	c[ImGuiCol_SliderGrabActive] = shadow;
	c[ImGuiCol_Button] = face;
	c[ImGuiCol_ButtonHovered] = rgb(204, 204, 204);
	c[ImGuiCol_ButtonActive] = shadow;
	c[ImGuiCol_Header] = navy;
	c[ImGuiCol_HeaderHovered] = rgb(166, 202, 240);
	c[ImGuiCol_HeaderActive] = navy;
	c[ImGuiCol_Separator] = shadow;
	c[ImGuiCol_SeparatorHovered] = shadow;
	c[ImGuiCol_SeparatorActive] = black;
	c[ImGuiCol_ResizeGrip] = shadow;
	c[ImGuiCol_ResizeGripHovered] = black;
	c[ImGuiCol_ResizeGripActive] = black;
	c[ImGuiCol_Tab] = rgb(176, 176, 176);
	c[ImGuiCol_TabHovered] = face;
	c[ImGuiCol_TabSelected] = face;
	c[ImGuiCol_TabSelectedOverline] = face;
	c[ImGuiCol_TabDimmed] = rgb(176, 176, 176);
	c[ImGuiCol_TabDimmedSelected] = face;
	c[ImGuiCol_PlotLines] = navy;
	c[ImGuiCol_PlotHistogram] = navy;
	c[ImGuiCol_TableHeaderBg] = face;
	c[ImGuiCol_TableBorderStrong] = shadow;
	c[ImGuiCol_TableBorderLight] = shadow;
	c[ImGuiCol_TableRowBgAlt] = rgb(0, 0, 0, 0.04f);
	c[ImGuiCol_TextSelectedBg] = rgb(166, 202, 240);
	c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.2f);
	c[ImGuiCol_NavHighlight] = black;
	s_base_style = style;
}

void apply_theme() {
	if (s_classic) {
		apply_classic_theme();
		return;
	}
	ImGuiStyle& style = ImGui::GetStyle();
	style = ImGuiStyle();
	// continuous, generous corners; capsule controls (rounding is capped at half the height)
	style.WindowRounding = 14;
	style.ChildRounding = 10;
	style.FrameRounding = 20;
	style.PopupRounding = 10;
	style.ScrollbarRounding = 10;
	style.GrabRounding = 20;
	style.TabRounding = 8;
	style.WindowBorderSize = 1;
	style.ChildBorderSize = 1;
	style.FrameBorderSize = 0;
	style.PopupBorderSize = 1;
	style.TabBorderSize = 0;
	style.WindowPadding = ImVec2(14, 12);
	style.FramePadding = ImVec2(12, 6);
	style.ItemSpacing = ImVec2(8, 8);
	style.ItemInnerSpacing = ImVec2(6, 4);
	style.CellPadding = ImVec2(8, 5);
	style.ScrollbarSize = 9;
	style.GrabMinSize = 12;
	style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
	style.SeparatorTextBorderSize = 1;
	style.SeparatorTextPadding = ImVec2(10, 4);

	// Apple's dark materials and grey text levels, with Albia's leaf green as the one accent
	ImVec4* c = style.Colors;
	c[ImGuiCol_Text] = rgb(245, 245, 247);
	c[ImGuiCol_TextDisabled] = rgb(152, 152, 157);
	c[ImGuiCol_WindowBg] = rgb(30, 30, 32, 0.94f);
	c[ImGuiCol_ChildBg] = rgb(255, 255, 255, 0.03f);
	c[ImGuiCol_PopupBg] = rgb(38, 38, 40, 0.98f);
	c[ImGuiCol_Border] = rgb(255, 255, 255, 0.10f);
	c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
	c[ImGuiCol_FrameBg] = rgb(118, 118, 128, 0.24f);
	c[ImGuiCol_FrameBgHovered] = rgb(118, 118, 128, 0.32f);
	c[ImGuiCol_FrameBgActive] = rgb(118, 118, 128, 0.40f);
	c[ImGuiCol_TitleBg] = rgb(30, 30, 32, 0.96f);
	c[ImGuiCol_TitleBgActive] = rgb(40, 40, 42, 0.98f);
	c[ImGuiCol_TitleBgCollapsed] = rgb(30, 30, 32, 0.8f);
	c[ImGuiCol_MenuBarBg] = rgb(28, 28, 30, 0.96f);
	c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab] = rgb(255, 255, 255, 0.18f);
	c[ImGuiCol_ScrollbarGrabHovered] = rgb(255, 255, 255, 0.28f);
	c[ImGuiCol_ScrollbarGrabActive] = rgb(255, 255, 255, 0.38f);
	c[ImGuiCol_CheckMark] = rgb(255, 255, 255);
	c[ImGuiCol_SliderGrab] = rgb(255, 255, 255, 0.9f);
	c[ImGuiCol_SliderGrabActive] = rgb(255, 255, 255);
	c[ImGuiCol_Button] = rgb(118, 118, 128, 0.24f);
	c[ImGuiCol_ButtonHovered] = rgb(118, 118, 128, 0.36f);
	c[ImGuiCol_ButtonActive] = rgb(118, 118, 128, 0.48f);
	c[ImGuiCol_Header] = rgb(110, 190, 90, 0.30f);
	c[ImGuiCol_HeaderHovered] = rgb(118, 118, 128, 0.30f);
	c[ImGuiCol_HeaderActive] = rgb(110, 190, 90, 0.45f);
	c[ImGuiCol_Separator] = rgb(255, 255, 255, 0.10f);
	c[ImGuiCol_SeparatorHovered] = rgb(255, 255, 255, 0.2f);
	c[ImGuiCol_SeparatorActive] = rgb(110, 190, 90);
	c[ImGuiCol_ResizeGrip] = rgb(255, 255, 255, 0.0f);
	c[ImGuiCol_ResizeGripHovered] = rgb(255, 255, 255, 0.15f);
	c[ImGuiCol_ResizeGripActive] = rgb(255, 255, 255, 0.3f);
	c[ImGuiCol_Tab] = rgb(118, 118, 128, 0.0f);
	c[ImGuiCol_TabHovered] = rgb(118, 118, 128, 0.30f);
	c[ImGuiCol_TabSelected] = rgb(118, 118, 128, 0.42f);
	c[ImGuiCol_TabSelectedOverline] = rgb(110, 190, 90);
	c[ImGuiCol_TabDimmed] = rgb(118, 118, 128, 0.0f);
	c[ImGuiCol_TabDimmedSelected] = rgb(118, 118, 128, 0.3f);
	c[ImGuiCol_PlotLines] = rgb(110, 190, 90);
	c[ImGuiCol_PlotHistogram] = rgb(110, 190, 90);
	c[ImGuiCol_TableHeaderBg] = rgb(255, 255, 255, 0.05f);
	c[ImGuiCol_TableBorderStrong] = rgb(255, 255, 255, 0.10f);
	c[ImGuiCol_TableBorderLight] = rgb(255, 255, 255, 0.06f);
	c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.03f);
	c[ImGuiCol_TextSelectedBg] = rgb(110, 190, 90, 0.35f);
	c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.45f);
	c[ImGuiCol_NavHighlight] = rgb(110, 190, 90);
	s_base_style = style;
}

ImU32 with_alpha(ImU32 colour, float alpha) {
	ImVec4 c = ImGui::ColorConvertU32ToFloat4(colour);
	c.w *= alpha;
	return ImGui::ColorConvertFloat4ToU32(c);
}

} // namespace

void init(bool main_window) {
	// Both modes' fonts are loaded at once, so switching modes needn't rebuild the font
	// texture in the middle of a frame.
	ImGuiIO& io = ImGui::GetIO();
	const char* text = find_font(false);
	const char* title = find_font(true);
	Fonts& fonts = s_fonts[ImGui::GetCurrentContext()];
	io.Fonts->Clear();
	fonts.classic = io.Fonts->AddFontDefault(); // ImGui's bitmap font: classic mode
	fonts.regular = fonts.title = nullptr;
	if (text) {
		ImFontConfig config;
		config.OversampleH = 2;
		config.OversampleV = 2;
		// Latin with its accents (names), and punctuation and arrows: the ellipsis that cuts
		// long names short, dashes and quotes
		static const ImWchar ranges[] = {0x0020, 0x017F, 0x2000, 0x206F, 0x2190, 0x2199, 0};
		fonts.regular = io.Fonts->AddFontFromFileTTF(text, FONT_SIZE * FONT_OVERSAMPLE, &config, ranges);
		if (fonts.regular) {
			fonts.title = io.Fonts->AddFontFromFileTTF(title ? title : text, TITLE_SIZE * FONT_OVERSAMPLE, &config, ranges);
		}
	}
	if (main_window) {
		sdl_imgui_rebuild_font_texture(fonts.regular != nullptr);
	}
	io.FontDefault = s_classic || !fonts.regular ? fonts.classic : fonts.regular;
	apply_theme();
	set_scale(s_scale);
}

void set_scale(float scale) {
	s_scale = std::clamp(scale, 0.75f, 2.0f);
	ImGuiStyle& style = ImGui::GetStyle();
	style = s_base_style;
	style.ScaleAllSizes(s_scale);
	ImGuiIO& io = ImGui::GetIO();
	const bool oversampled = io.FontDefault && io.FontDefault->FontSize > FONT_SIZE * 1.5f;
	io.FontGlobalScale = s_scale / (oversampled ? FONT_OVERSAMPLE : 1.0f);
}

void set_classic(bool classic) {
	if (classic == s_classic) {
		return;
	}
	s_classic = classic;
	auto it = s_fonts.find(ImGui::GetCurrentContext());
	if (it != s_fonts.end()) {
		ImGui::GetIO().FontDefault = s_classic || !it->second.regular ? it->second.classic : it->second.regular;
	}
	apply_theme();
	set_scale(s_scale);
}

bool classic() {
	return s_classic;
}

float scale() {
	return s_scale;
}

ImFont* title_font() {
	auto it = s_fonts.find(ImGui::GetCurrentContext());
	return it != s_fonts.end() && it->second.title && !s_classic ? it->second.title : ImGui::GetFont();
}

ImU32 accent() {
	return IM_COL32(110, 190, 90, 255);
}

ImU32 accent_soft() {
	return IM_COL32(110, 190, 90, 70);
}

ImU32 panel() {
	return IM_COL32(30, 30, 32, 240);
}

ImU32 text_dim() {
	return IM_COL32(152, 152, 157, 255);
}

ImU32 separator() {
	return IM_COL32(255, 255, 255, 26);
}

ImVec4 health_colour(float f) {
	// Apple's system red, orange and green
	if (f < 0.25f) {
		return rgb(255, 69, 58);
	}
	if (f < 0.5f) {
		return rgb(255, 159, 10);
	}
	return rgb(48, 209, 88);
}

void shadow(ImVec2 min, ImVec2 max, float rounding) {
	// the same soft shadow as the rest of the modern interface
	UIArt::shadow(ImGui::GetBackgroundDrawList(), min, max, rounding);
}

void window_shadow() {
	ImGuiWindow* window = ImGui::GetCurrentContext()->CurrentWindow;
	if (window) {
		shadow(window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y), ImGui::GetStyle().WindowRounding);
	}
}

void meter(const char* label, float fraction, ImVec4 colour, float width) {
	fraction = std::clamp(fraction, 0.f, 1.f);
	const float h = std::max(4.f, ImGui::GetTextLineHeight() * 0.42f);
	ImVec2 pos = ImGui::GetCursorScreenPos();
	pos.y += (ImGui::GetTextLineHeight() - h) / 2;
	auto* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + h), IM_COL32(118, 118, 128, 70), h / 2);
	if (fraction > 0) {
		draw->AddRectFilled(pos, ImVec2(pos.x + std::max(h, width * fraction), pos.y + h), ImGui::ColorConvertFloat4ToU32(colour), h / 2);
	}
	ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight()));
	if (label && label[0]) {
		ImGui::SameLine();
		ImGui::TextUnformatted(label);
	}
}

void draw_icon(ImDrawList* d, Icon icon, ImVec2 o, float s, ImU32 c) {
	const float t = std::max(1.5f, s * 0.09f); // line thickness
	auto p = [&](float x, float y) { return ImVec2(o.x + x * s, o.y + y * s); };
	switch (icon) {
		case Icon::Pause:
			d->AddRectFilled(p(0.26f, 0.2f), p(0.42f, 0.8f), c, s * 0.04f);
			d->AddRectFilled(p(0.58f, 0.2f), p(0.74f, 0.8f), c, s * 0.04f);
			break;
		case Icon::Play:
			d->AddTriangleFilled(p(0.3f, 0.18f), p(0.3f, 0.82f), p(0.82f, 0.5f), c);
			break;
		case Icon::Eye:
			d->PathArcTo(p(0.5f, 0.95f), s * 0.55f, IM_PI * 1.22f, IM_PI * 1.78f, 16);
			d->PathArcTo(p(0.5f, 0.05f), s * 0.55f, IM_PI * 0.22f, IM_PI * 0.78f, 16);
			d->PathStroke(c, ImDrawFlags_Closed, t);
			d->AddCircleFilled(p(0.5f, 0.5f), s * 0.15f, c);
			break;
		case Icon::Follow:
			d->AddCircle(p(0.5f, 0.5f), s * 0.34f, c, 24, t);
			d->AddCircleFilled(p(0.5f, 0.5f), s * 0.12f, c);
			d->AddLine(p(0.5f, 0.02f), p(0.5f, 0.2f), c, t);
			d->AddLine(p(0.5f, 0.8f), p(0.5f, 0.98f), c, t);
			d->AddLine(p(0.02f, 0.5f), p(0.2f, 0.5f), c, t);
			d->AddLine(p(0.8f, 0.5f), p(0.98f, 0.5f), c, t);
			break;
		case Icon::Egg:
			d->PathArcTo(p(0.5f, 0.6f), s * 0.3f, 0, IM_PI, 16);
			d->PathBezierCubicCurveTo(p(0.2f, 0.32f), p(0.36f, 0.08f), p(0.5f, 0.08f));
			d->PathBezierCubicCurveTo(p(0.64f, 0.08f), p(0.8f, 0.32f), p(0.8f, 0.6f));
			d->PathStroke(c, ImDrawFlags_Closed, t);
			break;
		case Icon::Kits:
			d->AddRect(p(0.12f, 0.32f), p(0.88f, 0.84f), c, s * 0.08f, 0, t);
			d->AddRect(p(0.36f, 0.16f), p(0.64f, 0.32f), c, s * 0.05f, 0, t);
			d->AddLine(p(0.12f, 0.54f), p(0.88f, 0.54f), c, t);
			break;
		case Icon::MedKit:
			// a first-aid box: handle, body, cross
			d->AddRect(p(0.38f, 0.14f), p(0.62f, 0.3f), c, s * 0.05f, 0, t);
			d->AddRect(p(0.1f, 0.3f), p(0.9f, 0.86f), c, s * 0.1f, 0, t);
			d->AddRectFilled(p(0.44f, 0.42f), p(0.56f, 0.74f), c);
			d->AddRectFilled(p(0.34f, 0.52f), p(0.66f, 0.64f), c);
			break;
		case Icon::Paw:
			// a wolf's paw print
			d->AddCircleFilled(p(0.5f, 0.66f), s * 0.17f, c, 20);
			d->AddCircleFilled(p(0.22f, 0.44f), s * 0.08f, c, 12);
			d->AddCircleFilled(p(0.38f, 0.26f), s * 0.085f, c, 12);
			d->AddCircleFilled(p(0.62f, 0.26f), s * 0.085f, c, 12);
			d->AddCircleFilled(p(0.78f, 0.44f), s * 0.08f, c, 12);
			break;
		case Icon::FamilyTree:
			// two parents joined above one child
			d->AddCircleFilled(p(0.24f, 0.22f), s * 0.11f, c, 14);
			d->AddCircleFilled(p(0.76f, 0.22f), s * 0.11f, c, 14);
			d->AddCircleFilled(p(0.5f, 0.8f), s * 0.11f, c, 14);
			d->AddLine(p(0.24f, 0.34f), p(0.24f, 0.5f), c, t);
			d->AddLine(p(0.76f, 0.34f), p(0.76f, 0.5f), c, t);
			d->AddLine(p(0.24f, 0.5f), p(0.76f, 0.5f), c, t);
			d->AddLine(p(0.5f, 0.5f), p(0.5f, 0.68f), c, t);
			break;
		case Icon::Places:
			d->PathArcTo(p(0.5f, 0.38f), s * 0.24f, IM_PI * 0.85f, IM_PI * 2.15f, 16);
			d->PathLineTo(p(0.5f, 0.92f));
			d->PathStroke(c, ImDrawFlags_Closed, t);
			d->AddCircleFilled(p(0.5f, 0.38f), s * 0.08f, c);
			break;
		case Icon::Teach:
			d->AddRect(p(0.1f, 0.2f), p(0.9f, 0.68f), c, s * 0.05f, 0, t);
			d->AddLine(p(0.22f, 0.36f), p(0.6f, 0.36f), c, t);
			d->AddLine(p(0.22f, 0.52f), p(0.48f, 0.52f), c, t);
			d->AddLine(p(0.3f, 0.68f), p(0.2f, 0.9f), c, t);
			d->AddLine(p(0.7f, 0.68f), p(0.8f, 0.9f), c, t);
			break;
		case Icon::SpeakerOn:
		case Icon::SpeakerOff:
			d->AddQuadFilled(p(0.1f, 0.38f), p(0.3f, 0.38f), p(0.3f, 0.62f), p(0.1f, 0.62f), c);
			d->AddTriangleFilled(p(0.28f, 0.38f), p(0.52f, 0.16f), p(0.52f, 0.5f), c);
			d->AddTriangleFilled(p(0.28f, 0.62f), p(0.52f, 0.5f), p(0.52f, 0.84f), c);
			if (icon == Icon::SpeakerOn) {
				d->PathArcTo(p(0.5f, 0.5f), s * 0.22f, -IM_PI * 0.3f, IM_PI * 0.3f, 10);
				d->PathStroke(c, 0, t);
				d->PathArcTo(p(0.5f, 0.5f), s * 0.38f, -IM_PI * 0.3f, IM_PI * 0.3f, 12);
				d->PathStroke(c, 0, t);
			} else {
				d->AddLine(p(0.64f, 0.36f), p(0.9f, 0.64f), c, t);
				d->AddLine(p(0.9f, 0.36f), p(0.64f, 0.64f), c, t);
			}
			break;
		case Icon::Say:
			d->AddRect(p(0.1f, 0.18f), p(0.9f, 0.7f), c, s * 0.22f, 0, t);
			d->AddTriangleFilled(p(0.26f, 0.66f), p(0.44f, 0.66f), p(0.22f, 0.88f), c);
			break;
		case Icon::History:
			d->AddCircle(p(0.5f, 0.5f), s * 0.38f, c, 24, t);
			d->AddLine(p(0.5f, 0.5f), p(0.5f, 0.26f), c, t);
			d->AddLine(p(0.5f, 0.5f), p(0.68f, 0.6f), c, t);
			break;
		case Icon::Camera:
			d->AddRect(p(0.1f, 0.3f), p(0.9f, 0.82f), c, s * 0.08f, 0, t);
			d->AddRectFilled(p(0.34f, 0.18f), p(0.62f, 0.3f), c, s * 0.04f);
			d->AddCircle(p(0.5f, 0.56f), s * 0.15f, c, 20, t);
			break;
	}
}

bool icon_button(const char* id, Icon icon, const char* label, const char* tooltip, bool on) {
	const ImGuiStyle& style = ImGui::GetStyle();
	const float h = ImGui::GetFrameHeight();
	const float icon_size = h * 0.56f;
	const float label_w = label && label[0] ? ImGui::CalcTextSize(label).x : 0;
	const float w = label_w > 0 ? style.FramePadding.x * 1.4f + icon_size + 6 + label_w + style.FramePadding.x : h;
	// wrap to a new row when it won't fit
	if (ImGui::GetContentRegionAvail().x < w && ImGui::GetCursorPosX() > style.WindowPadding.x + 1) {
		ImGui::NewLine();
	}
	ImVec2 pos = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();
	auto* d = ImGui::GetWindowDrawList();
	ImU32 bg = on ? accent() : IM_COL32(118, 118, 128, held ? 120 : hovered ? 92 : 62);
	if (on && (hovered || held)) {
		bg = with_alpha(bg, held ? 0.8f : 0.9f);
	}
	d->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), bg, h / 2);
	const ImU32 fg = on ? IM_COL32(16, 30, 12, 255) : IM_COL32(245, 245, 247, 255);
	const float ix = label_w > 0 ? pos.x + style.FramePadding.x * 1.0f : pos.x + (w - icon_size) / 2;
	draw_icon(d, icon, ImVec2(ix, pos.y + (h - icon_size) / 2), icon_size, fg);
	if (label_w > 0) {
		d->AddText(ImVec2(ix + icon_size + 6, pos.y + (h - ImGui::GetTextLineHeight()) / 2), fg, label);
	}
	if (tooltip && tooltip[0]) {
		ImGui::SetItemTooltip("%s", tooltip);
	}
	ImGui::SameLine();
	return pressed;
}

bool tool_button(const char* id, Icon icon, const char* tooltip, bool on) {
	const float size = ImGui::GetFrameHeight() * 1.55f;
	const float icon_size = size * 0.56f;
	if (ImGui::GetContentRegionAvail().x < size && ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x + 1) {
		ImGui::NewLine();
	}
	ImVec2 pos = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();
	const bool enabled = !(ImGui::GetItemFlags() & ImGuiItemFlags_Disabled);
	auto* d = ImGui::GetWindowDrawList();
	ImU32 bg = on ? accent() : IM_COL32(118, 118, 128, held ? 120 : hovered ? 92 : 62);
	if (on && (hovered || held)) {
		bg = with_alpha(bg, held ? 0.8f : 0.9f);
	}
	d->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), bg, size * 0.28f);
	ImU32 fg = on ? IM_COL32(16, 30, 12, 255) : IM_COL32(245, 245, 247, 255);
	if (!enabled) {
		fg = with_alpha(fg, 0.4f);
	}
	draw_icon(d, icon, ImVec2(pos.x + (size - icon_size) / 2, pos.y + (size - icon_size) / 2), icon_size, fg);
	if (tooltip && tooltip[0]) {
		ImGui::SetItemTooltip("%s", tooltip);
	}
	ImGui::SameLine();
	return pressed;
}

namespace {

// A plush toy Norn, drawn with shapes: a round purple body and head, long floppy ears, a pale
// belly and muzzle, button eyes and stitched seams.
void draw_plushie(ImDrawList* d, ImVec2 c, float s, bool on) {
	const float a = on ? 1.f : 0.6f;
	auto col = [&](int r, int g, int b) { return IM_COL32(r, g, b, static_cast<int>(255 * a)); };
	const ImU32 fur = col(146, 92, 204), fur_dark = col(98, 56, 150), belly = col(226, 204, 246);
	const ImU32 seam = col(70, 36, 110), eye = col(28, 18, 40), shine = IM_COL32(255, 255, 255, static_cast<int>(230 * a));
	// ears, hanging out and down from the top of the head
	for (float side : {-1.f, 1.f}) {
		const ImVec2 root(c.x + side * s * 0.16f, c.y - s * 0.28f);
		const ImVec2 tip(c.x + side * s * 0.46f, c.y - s * 0.02f);
		d->AddBezierQuadratic(root, ImVec2(c.x + side * s * 0.52f, c.y - s * 0.46f), tip, fur_dark, s * 0.13f, 12);
		d->AddCircleFilled(tip, s * 0.065f, fur_dark);
	}
	// feet, body and arms
	d->AddEllipseFilled(ImVec2(c.x - s * 0.14f, c.y + s * 0.40f), ImVec2(s * 0.11f, s * 0.07f), fur_dark);
	d->AddEllipseFilled(ImVec2(c.x + s * 0.14f, c.y + s * 0.40f), ImVec2(s * 0.11f, s * 0.07f), fur_dark);
	d->AddEllipseFilled(ImVec2(c.x, c.y + s * 0.20f), ImVec2(s * 0.25f, s * 0.22f), fur);
	d->AddEllipseFilled(ImVec2(c.x, c.y + s * 0.24f), ImVec2(s * 0.14f, s * 0.14f), belly);
	d->AddEllipseFilled(ImVec2(c.x - s * 0.24f, c.y + s * 0.18f), ImVec2(s * 0.07f, s * 0.11f), fur_dark);
	d->AddEllipseFilled(ImVec2(c.x + s * 0.24f, c.y + s * 0.18f), ImVec2(s * 0.07f, s * 0.11f), fur_dark);
	// head, with a pale muzzle
	const ImVec2 head(c.x, c.y - s * 0.12f);
	d->AddCircleFilled(head, s * 0.24f, fur, 28);
	d->AddEllipseFilled(ImVec2(head.x, head.y + s * 0.09f), ImVec2(s * 0.13f, s * 0.08f), belly);
	// button eyes with a shine, and a stitched nose
	for (float side : {-1.f, 1.f}) {
		const ImVec2 e(head.x + side * s * 0.095f, head.y - s * 0.02f);
		d->AddCircleFilled(e, s * 0.05f, eye, 14);
		d->AddCircleFilled(ImVec2(e.x - s * 0.015f, e.y - s * 0.017f), s * 0.016f, shine, 8);
	}
	d->AddTriangleFilled(ImVec2(head.x - s * 0.035f, head.y + s * 0.06f), ImVec2(head.x + s * 0.035f, head.y + s * 0.06f), ImVec2(head.x, head.y + s * 0.10f), seam);
	// stitched seams: down the middle of the belly, and a few across the top of the head
	const float t = std::max(1.f, s * 0.022f);
	for (int i = 0; i < 3; ++i) {
		const float y = c.y + s * (0.14f + 0.07f * static_cast<float>(i));
		d->AddLine(ImVec2(c.x, y), ImVec2(c.x, y + s * 0.035f), seam, t);
	}
	for (int i = -1; i <= 1; ++i) {
		const float x = head.x + static_cast<float>(i) * s * 0.07f;
		d->AddLine(ImVec2(x - s * 0.02f, head.y - s * 0.20f), ImVec2(x + s * 0.02f, head.y - s * 0.17f), seam, t);
	}
}

} // namespace

bool plushie_button(const char* id, const char* tooltip, bool on, float size) {
	if (size <= 0) {
		size = ImGui::GetFrameHeight() * 1.55f;
		if (ImGui::GetContentRegionAvail().x < size && ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x + 1) {
			ImGui::NewLine();
		}
	}
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();
	auto* d = ImGui::GetWindowDrawList();
	const float rounding = size * 0.28f;
	if (on) {
		// a soft purple glow round the button
		for (int i = 3; i >= 1; --i) {
			const float g = static_cast<float>(i) * size * 0.05f;
			d->AddRectFilled(ImVec2(pos.x - g, pos.y - g), ImVec2(pos.x + size + g, pos.y + size + g), IM_COL32(176, 120, 255, 26), rounding + g);
		}
	}
	const ImU32 bg = on ? IM_COL32(236, 222, 255, held ? 200 : 240) : IM_COL32(118, 118, 128, held ? 120 : hovered ? 92 : 62);
	d->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), bg, rounding);
	const float squash = held ? 0.92f : 1.f; // squeezed when pressed, like a soft toy
	draw_plushie(d, ImVec2(pos.x + size / 2, pos.y + size * 0.52f), size * 0.82f * squash, on);
	if (on) {
		// a little sparkle
		const ImVec2 sp(pos.x + size * 0.80f, pos.y + size * 0.20f);
		const float r = size * 0.07f;
		d->AddLine(ImVec2(sp.x - r, sp.y), ImVec2(sp.x + r, sp.y), IM_COL32(255, 255, 255, 230), 1.5f);
		d->AddLine(ImVec2(sp.x, sp.y - r), ImVec2(sp.x, sp.y + r), IM_COL32(255, 255, 255, 230), 1.5f);
	}
	if (tooltip && tooltip[0]) {
		ImGui::SetItemTooltip("%s", tooltip);
	}
	ImGui::SameLine();
	return pressed;
}

bool segmented(const char* id, const char* const* labels, int count, int* selected) {
	ImGui::PushID(id);
	const float h = ImGui::GetFrameHeight() * 0.86f;
	float w = 0;
	for (int i = 0; i < count; ++i) {
		w = std::max(w, ImGui::CalcTextSize(labels[i]).x + ImGui::GetStyle().FramePadding.x * 1.5f);
	}
	ImVec2 pos = ImGui::GetCursorScreenPos();
	pos.y += (ImGui::GetFrameHeight() - h) / 2;
	auto* d = ImGui::GetWindowDrawList();
	d->AddRectFilled(pos, ImVec2(pos.x + w * static_cast<float>(count) + 4, pos.y + h), IM_COL32(118, 118, 128, 62), h / 2);
	bool changed = false;
	for (int i = 0; i < count; ++i) {
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 2 + w * static_cast<float>(i), pos.y));
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("seg", ImVec2(w, h)) && *selected != i) {
			*selected = i;
			changed = true;
		}
		ImGui::PopID();
		const ImVec2 a(pos.x + 2 + w * static_cast<float>(i), pos.y + 2), b(a.x + w, pos.y + h - 2);
		if (*selected == i) {
			d->AddRectFilled(a, b, IM_COL32(99, 99, 102, 255), (h - 4) / 2);
		}
		const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
		d->AddText(ImVec2(a.x + (w - ts.x) / 2, pos.y + (h - ts.y) / 2), *selected == i ? IM_COL32(255, 255, 255, 255) : text_dim(), labels[i]);
	}
	ImGui::SetCursorScreenPos(ImVec2(pos.x + w * static_cast<float>(count) + 4, pos.y - (ImGui::GetFrameHeight() - h) / 2));
	ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
	ImGui::PopID();
	return changed;
}

} // namespace UIStyle
