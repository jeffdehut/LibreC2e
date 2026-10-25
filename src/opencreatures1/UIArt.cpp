#include "UIArt.h"

#include "common/backend/Backend.h"
#include "sdlbackend/SDLBackend.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace UIArt {

namespace {

constexpr float PI = 3.14159265f;

// --- the wood grain, made once ---

float hash(int x, int y) {
	uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return static_cast<float>((h ^ (h >> 16)) & 0xffffffu) / static_cast<float>(0xffffffu);
}

float smooth_noise(float x, float y) {
	const float fx = std::floor(x), fy = std::floor(y);
	const int xi = static_cast<int>(fx), yi = static_cast<int>(fy);
	float tx = x - fx, ty = y - fy;
	tx = tx * tx * (3 - 2 * tx);
	ty = ty * ty * (3 - 2 * ty);
	const float a = hash(xi, yi), b = hash(xi + 1, yi), c = hash(xi, yi + 1), d = hash(xi + 1, yi + 1);
	return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

float fbm(float x, float y) {
	float sum = 0, amplitude = 0.5f;
	for (int octave = 0; octave < 4; ++octave) {
		sum += smooth_noise(x, y) * amplitude;
		x *= 2.03f;
		y *= 2.03f;
		amplitude *= 0.5f;
	}
	return sum;
}

constexpr int WOOD_W = 2048, WOOD_H = 160;

ImTextureID wood_texture() {
	static ImTextureID texture{};
	static bool tried = false;
	if (tried) {
		return texture;
	}
	tried = true;
	auto* backend = dynamic_cast<SDLBackend*>(get_backend());
	SDL_Renderer* renderer = backend && backend->window ? SDL_GetRenderer(backend->window) : nullptr;
	if (!renderer) {
		return texture;
	}
	// grain running along the plank, waving round knots, with fine streaks and pores
	std::vector<uint8_t> rgba(static_cast<size_t>(WOOD_W) * WOOD_H * 4);
	for (int y = 0; y < WOOD_H; ++y) {
		for (int x = 0; x < WOOD_W; ++x) {
			const float fx = static_cast<float>(x), fy = static_cast<float>(y);
			const float warp = fbm(fx / 260.f, fy / 30.f) * 10 + fbm(fx / 46.f, fy / 8.f) * 2.6f;
			const float rings = 0.5f + 0.5f * std::sin((fy * 0.21f + warp) * 1.65f);
			const float streak = fbm(fx / 5.f, fy / 1.1f);
			const float pores = hash(x / 2, y) > 0.985f ? -0.18f : 0.f;
			float t = 0.30f + 0.42f * rings + 0.28f * (streak - 0.5f) + pores;
			t = std::clamp(t, 0.f, 1.f);
			const float r = 66 + (152 - 66) * t, g = 38 + (98 - 38) * t, b = 20 + (56 - 20) * t;
			uint8_t* p = &rgba[(static_cast<size_t>(y) * WOOD_W + static_cast<size_t>(x)) * 4];
			p[0] = static_cast<uint8_t>(r);
			p[1] = static_cast<uint8_t>(g);
			p[2] = static_cast<uint8_t>(b);
			p[3] = 255;
		}
	}
	SDL_Texture* sdl = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, WOOD_W, WOOD_H);
	if (!sdl) {
		return texture;
	}
	SDL_UpdateTexture(sdl, nullptr, rgba.data(), WOOD_W * 4);
	SDL_SetTextureBlendMode(sdl, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(sdl, SDL_SCALEMODE_LINEAR);
	texture = (ImTextureID)(intptr_t)sdl;
	return texture;
}

ImU32 scale_alpha(ImU32 colour, float a) {
	const ImU32 alpha = static_cast<ImU32>(static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.f, 1.f));
	return (colour & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

ImVec2 add(ImVec2 a, ImVec2 b) {
	return ImVec2(a.x + b.x, a.y + b.y);
}

ImVec2 polar(ImVec2 c, float r, float angle) {
	return ImVec2(c.x + std::cos(angle) * r, c.y + std::sin(angle) * r);
}

void arc(ImDrawList* d, ImVec2 c, float r, float from, float to, ImU32 colour, float thickness) {
	d->PathClear();
	d->PathArcTo(c, r, from, to, 24);
	d->PathStroke(colour, 0, thickness);
}

} // namespace

// a few faint layers, each a little wider and lower: together a soft edge, never a dark band
void shadow(ImDrawList* d, ImVec2 a, ImVec2 b, float rounding) {
	for (int i = 1; i <= 4; ++i) {
		const float spread = static_cast<float>(i);
		const float drop = 1.5f + 0.5f * static_cast<float>(i);
		d->AddRectFilled(ImVec2(a.x - spread, a.y - spread + drop), ImVec2(b.x + spread, b.y + spread + drop), IM_COL32(0, 0, 0, 22 - 4 * i), rounding + spread);
	}
}

void shadow_circle(ImDrawList* d, ImVec2 c, float r) {
	for (int i = 1; i <= 4; ++i) {
		const float spread = static_cast<float>(i);
		d->AddCircleFilled(ImVec2(c.x, c.y + 1.5f + 0.5f * static_cast<float>(i)), r + spread, IM_COL32(0, 0, 0, 22 - 4 * i), 40);
	}
}

void wood_panel(ImDrawList* d, ImVec2 a, ImVec2 b, float rounding, float darken) {
	const float w = b.x - a.x, h = b.y - a.y;
	if (w <= 0 || h <= 0) {
		return;
	}
	shadow(d, a, b, rounding);
	const int shade = static_cast<int>(255 * (1 - std::clamp(darken, 0.f, 0.9f)));
	if (ImTextureID wood = wood_texture()) {
		const float u = std::min(1.f, w / static_cast<float>(WOOD_W));
		const float v = std::min(1.f, h / static_cast<float>(WOOD_H));
		d->AddImageRounded(wood, a, b, ImVec2(0, 0), ImVec2(u, v), IM_COL32(shade, shade, shade, 255), rounding);
	} else {
		d->AddRectFilled(a, b, IM_COL32(110 * shade / 255, 68 * shade / 255, 36 * shade / 255, 255), rounding);
	}
	// bevel: light along the top edge, dark along the bottom, then a carved groove inside
	d->AddRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), IM_COL32(255, 226, 170, 46), rounding, 0, 1.5f);
	d->AddLine(ImVec2(a.x + rounding * 0.6f, b.y - 1.5f), ImVec2(b.x - rounding * 0.6f, b.y - 1.5f), IM_COL32(20, 10, 4, 150), 2);
	d->AddRect(a, b, IM_COL32(26, 14, 6, 230), rounding, 0, 1.5f);
	const float inset = std::min(5.f, std::min(w, h) * 0.12f);
	if (inset >= 3) {
		d->AddRect(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset), IM_COL32(20, 10, 4, 80), std::max(0.f, rounding - inset), 0, 1.2f);
		d->AddRect(ImVec2(a.x + inset + 1, a.y + inset + 1), ImVec2(b.x - inset + 1, b.y - inset + 1), IM_COL32(255, 220, 160, 26), std::max(0.f, rounding - inset), 0, 1);
	}
}

void brass_plate(ImDrawList* d, ImVec2 a, ImVec2 b, float rounding) {
	shadow(d, a, b, rounding);
	d->AddRectFilled(a, b, BRASS, rounding);
	const float mid = (a.y + b.y) / 2;
	d->AddRectFilled(a, ImVec2(b.x, mid), scale_alpha(BRASS_LIGHT, 0.45f), rounding, ImDrawFlags_RoundCornersTop);
	d->AddLine(ImVec2(a.x + rounding, a.y + 1.5f), ImVec2(b.x - rounding, a.y + 1.5f), scale_alpha(BRASS_LIGHT, 0.9f), 1.2f);
	d->AddRect(a, b, BRASS_DARK, rounding, 0, 1.2f);
}

void brass_ring(ImDrawList* d, ImVec2 c, float outer, float inner) {
	const float mid = (outer + inner) / 2, band = outer - inner;
	d->AddCircle(c, mid, BRASS, 48, band);
	// light from the top left, shade at the bottom right
	arc(d, c, mid + band * 0.12f, PI * 1.05f, PI * 1.62f, scale_alpha(BRASS_LIGHT, 0.85f), band * 0.42f);
	arc(d, c, mid - band * 0.1f, PI * 0.05f, PI * 0.62f, scale_alpha(BRASS_DARK, 0.75f), band * 0.42f);
	d->AddCircle(c, outer, IM_COL32(40, 26, 8, 230), 48, 1.4f);
	d->AddCircle(c, inner, IM_COL32(40, 26, 8, 200), 48, 1.2f);
}

void rivet(ImDrawList* d, ImVec2 c, float r) {
	d->AddCircleFilled(ImVec2(c.x + r * 0.15f, c.y + r * 0.25f), r, IM_COL32(20, 12, 4, 150), 12);
	d->AddCircleFilled(c, r, BRASS_DARK, 12);
	d->AddCircleFilled(ImVec2(c.x - r * 0.12f, c.y - r * 0.12f), r * 0.78f, BRASS, 12);
	d->AddCircleFilled(ImVec2(c.x - r * 0.32f, c.y - r * 0.32f), r * 0.3f, BRASS_LIGHT, 8);
}

void glass(ImDrawList* d, ImVec2 c, float r) {
	for (int i = 0; i < 7; ++i) {
		const float t = static_cast<float>(i) / 6.f;
		const int R = static_cast<int>(12 + 22 * t), G = static_cast<int>(16 + 28 * t), B = static_cast<int>(18 + 26 * t);
		d->AddCircleFilled(ImVec2(c.x - r * 0.08f * t, c.y - r * 0.1f * t), r * (1 - 0.13f * static_cast<float>(i)), IM_COL32(R, G, B, 255), 40);
	}
}

void glass_shine(ImDrawList* d, ImVec2 c, float r) {
	arc(d, c, r * 0.8f, PI * 1.08f, PI * 1.42f, IM_COL32(255, 255, 255, 70), r * 0.1f);
	d->AddCircleFilled(polar(c, r * 0.72f, PI * 1.55f), r * 0.05f, IM_COL32(255, 255, 255, 110), 8);
	d->AddCircle(c, r, IM_COL32(0, 0, 0, 120), 40, 2);
}

void ring_meter(ImDrawList* d, ImVec2 c, float r, float thickness, float fraction, ImU32 colour) {
	d->AddCircle(c, r, IM_COL32(10, 8, 6, 170), 48, thickness + 2);
	fraction = std::clamp(fraction, 0.f, 1.f);
	if (fraction > 0) {
		const float start = PI * 0.5f; // from the bottom, round to the left
		d->PathClear();
		d->PathArcTo(c, r, start, start + fraction * 2 * PI, 48);
		d->PathStroke(colour, 0, thickness);
		d->PathClear();
		d->PathArcTo(c, r - thickness * 0.2f, start, start + fraction * 2 * PI, 48);
		d->PathStroke(IM_COL32(255, 255, 255, 50), 0, thickness * 0.3f);
	}
}

void leaf(ImDrawList* d, ImVec2 base, float length, float angle, ImU32 colour) {
	const ImVec2 dir(std::cos(angle), std::sin(angle)), side(-dir.y, dir.x);
	const ImVec2 tip(base.x + dir.x * length, base.y + dir.y * length);
	const float w = length * 0.3f;
	const ImVec2 c1(base.x + dir.x * length * 0.45f + side.x * w, base.y + dir.y * length * 0.45f + side.y * w);
	const ImVec2 c2(base.x + dir.x * length * 0.45f - side.x * w, base.y + dir.y * length * 0.45f - side.y * w);
	d->PathClear();
	d->PathLineTo(base);
	d->PathBezierQuadraticCurveTo(c1, tip, 10);
	d->PathBezierQuadraticCurveTo(c2, base, 10);
	d->PathFillConvex(colour);
	d->AddLine(base, ImVec2(base.x + dir.x * length * 0.85f, base.y + dir.y * length * 0.85f), IM_COL32(30, 60, 20, 160), std::max(1.f, length * 0.05f));
}

void text(ImDrawList* d, ImVec2 pos, float size, ImU32 colour, const char* s, bool shadow) {
	ImFont* font = ImGui::GetFont();
	if (shadow) {
		for (const ImVec2 o : {ImVec2(1, 1), ImVec2(-1, 1), ImVec2(1, -1), ImVec2(0, 1.5f)}) {
			d->AddText(font, size, add(pos, o), IM_COL32(16, 8, 2, 190), s);
		}
	}
	d->AddText(font, size, pos, colour, s);
}

// --- the pictures ---

namespace {

void carrot(ImDrawList* d, ImVec2 c, float s) {
	// from the top right down to the bottom left
	const ImVec2 top(c.x + s * 0.22f, c.y - s * 0.22f), tip(c.x - s * 0.34f, c.y + s * 0.34f);
	const ImVec2 side(s * 0.13f, s * 0.13f);
	d->AddTriangleFilled(ImVec2(top.x - side.x, top.y + side.y * -1 + s * 0.02f), ImVec2(top.x + side.x, top.y + side.y), tip, IM_COL32(236, 128, 34, 255));
	d->AddTriangleFilled(ImVec2(top.x - side.x * 0.6f, top.y - side.y * 0.6f + s * 0.02f), ImVec2(top.x + side.x * 0.6f, top.y + side.y * 0.6f), tip, IM_COL32(252, 160, 60, 255));
	for (float t : {0.3f, 0.5f, 0.7f}) {
		const ImVec2 p(top.x + (tip.x - top.x) * t, top.y + (tip.y - top.y) * t);
		d->AddLine(ImVec2(p.x - s * 0.05f, p.y - s * 0.05f), ImVec2(p.x + s * 0.03f, p.y + s * 0.03f), IM_COL32(170, 80, 20, 220), std::max(1.f, s * 0.03f));
	}
	leaf(d, top, s * 0.3f, -PI * 0.35f, IM_COL32(96, 170, 60, 255));
	leaf(d, top, s * 0.26f, -PI * 0.1f, IM_COL32(120, 190, 70, 255));
	leaf(d, top, s * 0.24f, -PI * 0.62f, IM_COL32(80, 150, 50, 255));
}

void cheese(ImDrawList* d, ImVec2 c, float s) {
	const ImVec2 a(c.x - s * 0.24f, c.y + s * 0.18f), b(c.x + s * 0.26f, c.y + s * 0.18f), t(c.x + s * 0.2f, c.y - s * 0.16f);
	d->AddTriangleFilled(a, b, t, IM_COL32(250, 210, 80, 255));
	d->AddQuadFilled(a, b, ImVec2(b.x, b.y + s * 0.1f), ImVec2(a.x, a.y + s * 0.1f), IM_COL32(220, 170, 50, 255));
	d->AddCircleFilled(ImVec2(c.x + s * 0.08f, c.y + s * 0.07f), s * 0.045f, IM_COL32(200, 150, 40, 255), 10);
	d->AddCircleFilled(ImVec2(c.x + s * 0.16f, c.y - s * 0.03f), s * 0.03f, IM_COL32(200, 150, 40, 255), 10);
	d->AddTriangle(a, b, t, IM_COL32(120, 80, 20, 200), 1);
}

void hand(ImDrawList* d, ImVec2 c, float s, ImU32 skin) {
	// a little open hand: palm and four fingers, thumb to the left
	d->AddCircleFilled(ImVec2(c.x, c.y + s * 0.08f), s * 0.2f, skin, 20);
	for (int i = 0; i < 4; ++i) {
		const float x = c.x - s * 0.15f + static_cast<float>(i) * s * 0.1f;
		d->AddRectFilled(ImVec2(x - s * 0.04f, c.y - s * 0.28f + (i == 0 || i == 3 ? s * 0.06f : 0)), ImVec2(x + s * 0.04f, c.y), skin, s * 0.04f);
	}
	d->AddRectFilled(ImVec2(c.x - s * 0.32f, c.y), ImVec2(c.x - s * 0.14f, c.y + s * 0.08f), skin, s * 0.04f);
}

} // namespace

void picture(ImDrawList* d, Picture p, ImVec2 c, float s, bool lit) {
	const ImU32 metal = lit ? BRASS_LIGHT : IM_COL32(226, 196, 130, 255);
	const float line = std::max(1.5f, s * 0.07f);
	switch (p) {
		case Picture::Give:
			cheese(d, ImVec2(c.x + s * 0.12f, c.y + s * 0.1f), s * 0.8f);
			carrot(d, ImVec2(c.x - s * 0.05f, c.y - s * 0.02f), s);
			break;
		case Picture::Heal: {
			// a round-bellied bottle of green medicine with a cork and a red cross, and herbs
			const ImVec2 body(c.x - s * 0.04f, c.y + s * 0.1f);
			const float r = s * 0.24f;
			d->AddRectFilled(ImVec2(body.x - s * 0.07f, c.y - s * 0.3f), ImVec2(body.x + s * 0.07f, body.y - r * 0.7f), IM_COL32(170, 210, 220, 200), s * 0.03f);
			d->AddRectFilled(ImVec2(body.x - s * 0.08f, c.y - s * 0.38f), ImVec2(body.x + s * 0.08f, c.y - s * 0.28f), IM_COL32(170, 110, 60, 255), s * 0.03f);
			d->AddCircleFilled(body, r, IM_COL32(170, 210, 220, 210), 28);
			d->AddCircleFilled(ImVec2(body.x, body.y + r * 0.18f), r * 0.8f, IM_COL32(80, 180, 90, 235), 28);
			d->AddRectFilled(ImVec2(body.x - r * 0.5f, body.y - r * 0.32f), ImVec2(body.x + r * 0.5f, body.y + r * 0.32f), IM_COL32(250, 244, 230, 255), 2);
			d->AddRectFilled(ImVec2(body.x - r * 0.1f, body.y - r * 0.26f), ImVec2(body.x + r * 0.1f, body.y + r * 0.26f), IM_COL32(210, 40, 40, 255));
			d->AddRectFilled(ImVec2(body.x - r * 0.3f, body.y - r * 0.08f), ImVec2(body.x + r * 0.3f, body.y + r * 0.08f), IM_COL32(210, 40, 40, 255));
			d->AddCircle(body, r, IM_COL32(40, 60, 70, 200), 28, 1.2f);
			leaf(d, ImVec2(c.x + s * 0.3f, c.y + s * 0.34f), s * 0.3f, -PI * 0.62f, IM_COL32(90, 160, 60, 255));
			leaf(d, ImVec2(c.x + s * 0.3f, c.y + s * 0.34f), s * 0.24f, -PI * 0.35f, IM_COL32(120, 190, 70, 255));
			break;
		}
		case Picture::Teach: {
			// a speech scroll with letters
			const ImVec2 a(c.x - s * 0.38f, c.y - s * 0.28f), b(c.x + s * 0.38f, c.y + s * 0.18f);
			d->AddRectFilled(a, b, IM_COL32(246, 232, 196, 255), s * 0.1f);
			d->AddTriangleFilled(ImVec2(c.x - s * 0.18f, b.y - 1), ImVec2(c.x - s * 0.02f, b.y - 1), ImVec2(c.x - s * 0.24f, c.y + s * 0.38f), IM_COL32(246, 232, 196, 255));
			d->AddRect(a, b, IM_COL32(120, 84, 40, 255), s * 0.1f, 0, 1.2f);
			ImFont* font = ImGui::GetFont();
			const float size = s * 0.34f;
			const char* letters = "Abc";
			const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, letters);
			d->AddText(font, size, ImVec2(c.x - ts.x / 2, (a.y + b.y) / 2 - ts.y / 2), IM_COL32(90, 50, 20, 255), letters);
			break;
		}
		case Picture::Norn: {
			const ImU32 fur = IM_COL32(196, 128, 64, 255), dark = IM_COL32(110, 62, 28, 255);
			d->AddEllipseFilled(ImVec2(c.x - s * 0.3f, c.y - s * 0.12f), ImVec2(s * 0.12f, s * 0.2f), dark, -0.5f);
			d->AddEllipseFilled(ImVec2(c.x + s * 0.3f, c.y - s * 0.12f), ImVec2(s * 0.12f, s * 0.2f), dark, 0.5f);
			d->AddCircleFilled(ImVec2(c.x, c.y + s * 0.04f), s * 0.28f, fur, 24);
			d->AddEllipseFilled(ImVec2(c.x, c.y + s * 0.16f), ImVec2(s * 0.16f, s * 0.1f), IM_COL32(236, 196, 150, 255));
			for (float side : {-1.f, 1.f}) {
				d->AddCircleFilled(ImVec2(c.x + side * s * 0.11f, c.y - s * 0.02f), s * 0.075f, IM_COL32(255, 255, 255, 255), 12);
				d->AddCircleFilled(ImVec2(c.x + side * s * 0.1f, c.y - s * 0.01f), s * 0.04f, IM_COL32(30, 20, 10, 255), 10);
			}
			d->AddCircleFilled(ImVec2(c.x, c.y + s * 0.12f), s * 0.035f, dark, 8);
			break;
		}
		case Picture::Lab: {
			// a flask with bubbling blue liquid
			const ImVec2 n0(c.x - s * 0.07f, c.y - s * 0.34f), n1(c.x + s * 0.07f, c.y - s * 0.1f);
			d->AddRectFilled(n0, n1, IM_COL32(190, 220, 230, 200));
			d->AddTriangleFilled(ImVec2(c.x - s * 0.07f, c.y - s * 0.12f), ImVec2(c.x + s * 0.07f, c.y - s * 0.12f), ImVec2(c.x, c.y - s * 0.1f), IM_COL32(190, 220, 230, 200));
			d->AddQuadFilled(ImVec2(c.x - s * 0.07f, c.y - s * 0.1f), ImVec2(c.x + s * 0.07f, c.y - s * 0.1f), ImVec2(c.x + s * 0.32f, c.y + s * 0.32f), ImVec2(c.x - s * 0.32f, c.y + s * 0.32f), IM_COL32(190, 220, 230, 200));
			d->AddQuadFilled(ImVec2(c.x - s * 0.17f, c.y + s * 0.07f), ImVec2(c.x + s * 0.17f, c.y + s * 0.07f), ImVec2(c.x + s * 0.3f, c.y + s * 0.3f), ImVec2(c.x - s * 0.3f, c.y + s * 0.3f), IM_COL32(70, 150, 220, 240));
			d->AddCircleFilled(ImVec2(c.x - s * 0.06f, c.y + s * 0.14f), s * 0.035f, IM_COL32(220, 240, 255, 220), 8);
			d->AddCircleFilled(ImVec2(c.x + s * 0.08f, c.y + s * 0.2f), s * 0.025f, IM_COL32(220, 240, 255, 220), 8);
			d->AddRectFilled(ImVec2(c.x - s * 0.1f, c.y - s * 0.4f), ImVec2(c.x + s * 0.1f, c.y - s * 0.33f), metal, 2);
			break;
		}
		case Picture::Objects: {
			// a wooden crate
			const ImVec2 a(c.x - s * 0.3f, c.y - s * 0.26f), b(c.x + s * 0.3f, c.y + s * 0.3f);
			d->AddRectFilled(a, b, IM_COL32(176, 118, 62, 255), 3);
			d->AddRect(a, b, IM_COL32(90, 54, 24, 255), 3, 0, line);
			d->AddLine(a, b, IM_COL32(110, 66, 30, 255), line);
			d->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, a.y), IM_COL32(110, 66, 30, 255), line);
			d->AddLine(ImVec2(a.x, (a.y + b.y) / 2), ImVec2(b.x, (a.y + b.y) / 2), IM_COL32(130, 80, 36, 200), line * 0.6f);
			break;
		}
		case Picture::Records: {
			// a leather book with brass corners
			const ImVec2 a(c.x - s * 0.26f, c.y - s * 0.32f), b(c.x + s * 0.26f, c.y + s * 0.32f);
			d->AddRectFilled(ImVec2(a.x + s * 0.06f, a.y + s * 0.03f), ImVec2(b.x + s * 0.03f, b.y + s * 0.03f), IM_COL32(244, 232, 200, 255), 2);
			d->AddRectFilled(a, b, IM_COL32(130, 50, 40, 255), 3);
			d->AddRect(a, b, IM_COL32(70, 24, 18, 255), 3, 0, 1.2f);
			d->AddLine(ImVec2(a.x + s * 0.08f, a.y), ImVec2(a.x + s * 0.08f, b.y), IM_COL32(80, 28, 22, 255), line);
			d->AddRectFilled(ImVec2(c.x - s * 0.08f, c.y - s * 0.12f), ImVec2(c.x + s * 0.16f, c.y - s * 0.04f), metal, 1);
			for (const ImVec2 corner : {ImVec2(b.x, a.y), b}) {
				d->AddCircleFilled(corner, s * 0.06f, metal, 8);
			}
			break;
		}
		case Picture::Follow:
			d->AddCircle(c, s * 0.28f, metal, 24, line);
			d->AddCircleFilled(c, s * 0.1f, metal, 16);
			for (int i = 0; i < 4; ++i) {
				const float a = static_cast<float>(i) * PI / 2;
				d->AddLine(polar(c, s * 0.3f, a), polar(c, s * 0.42f, a), metal, line);
			}
			break;
		case Picture::Egg:
			d->AddEllipseFilled(ImVec2(c.x, c.y + s * 0.03f), ImVec2(s * 0.24f, s * 0.32f), IM_COL32(244, 232, 206, 255), 0, 24);
			for (const ImVec2 spot : {ImVec2(-0.08f, -0.1f), ImVec2(0.1f, 0.04f), ImVec2(-0.04f, 0.16f), ImVec2(0.06f, -0.2f)}) {
				d->AddCircleFilled(ImVec2(c.x + spot.x * s, c.y + spot.y * s), s * 0.035f, IM_COL32(160, 120, 80, 255), 8);
			}
			d->AddEllipse(ImVec2(c.x, c.y + s * 0.03f), ImVec2(s * 0.24f, s * 0.32f), IM_COL32(120, 90, 60, 200), 0, 24, 1.2f);
			break;
		case Picture::Places: {
			// a map pin
			const ImVec2 head(c.x, c.y - s * 0.1f);
			d->AddTriangleFilled(ImVec2(c.x - s * 0.17f, c.y - s * 0.02f), ImVec2(c.x + s * 0.17f, c.y - s * 0.02f), ImVec2(c.x, c.y + s * 0.38f), IM_COL32(200, 50, 40, 255));
			d->AddCircleFilled(head, s * 0.22f, IM_COL32(220, 64, 50, 255), 20);
			d->AddCircleFilled(head, s * 0.08f, IM_COL32(250, 236, 210, 255), 12);
			break;
		}
		case Picture::Voices:
		case Picture::Quiet: {
			d->AddRectFilled(ImVec2(c.x - s * 0.3f, c.y - s * 0.1f), ImVec2(c.x - s * 0.14f, c.y + s * 0.1f), metal, 1);
			d->AddTriangleFilled(ImVec2(c.x - s * 0.16f, c.y), ImVec2(c.x + s * 0.06f, c.y - s * 0.26f), ImVec2(c.x + s * 0.06f, c.y + s * 0.26f), metal);
			if (p == Picture::Voices) {
				arc(d, ImVec2(c.x + s * 0.06f, c.y), s * 0.16f, -PI * 0.3f, PI * 0.3f, metal, line);
				arc(d, ImVec2(c.x + s * 0.06f, c.y), s * 0.3f, -PI * 0.3f, PI * 0.3f, metal, line);
			} else {
				d->AddLine(ImVec2(c.x + s * 0.14f, c.y - s * 0.12f), ImVec2(c.x + s * 0.36f, c.y + s * 0.12f), IM_COL32(220, 80, 60, 255), line);
				d->AddLine(ImVec2(c.x + s * 0.14f, c.y + s * 0.12f), ImVec2(c.x + s * 0.36f, c.y - s * 0.12f), IM_COL32(220, 80, 60, 255), line);
			}
			break;
		}
		case Picture::More:
			for (int i = -1; i <= 1; ++i) {
				d->AddCircleFilled(ImVec2(c.x + static_cast<float>(i) * s * 0.22f, c.y), s * 0.07f, metal, 10);
			}
			break;
		case Picture::Tickle:
			hand(d, ImVec2(c.x - s * 0.08f, c.y + s * 0.04f), s, IM_COL32(246, 214, 170, 255));
			// a heart
			d->AddCircleFilled(ImVec2(c.x + s * 0.2f, c.y - s * 0.22f), s * 0.08f, IM_COL32(236, 90, 130, 255), 12);
			d->AddCircleFilled(ImVec2(c.x + s * 0.32f, c.y - s * 0.22f), s * 0.08f, IM_COL32(236, 90, 130, 255), 12);
			d->AddTriangleFilled(ImVec2(c.x + s * 0.13f, c.y - s * 0.19f), ImVec2(c.x + s * 0.39f, c.y - s * 0.19f), ImVec2(c.x + s * 0.26f, c.y - s * 0.02f), IM_COL32(236, 90, 130, 255));
			break;
		case Picture::NewWorld: {
			// a seedling in a little pot
			d->AddQuadFilled(ImVec2(c.x - s * 0.2f, c.y + s * 0.08f), ImVec2(c.x + s * 0.2f, c.y + s * 0.08f), ImVec2(c.x + s * 0.15f, c.y + s * 0.36f), ImVec2(c.x - s * 0.15f, c.y + s * 0.36f), IM_COL32(190, 96, 56, 255));
			d->AddRectFilled(ImVec2(c.x - s * 0.23f, c.y + s * 0.03f), ImVec2(c.x + s * 0.23f, c.y + s * 0.1f), IM_COL32(160, 76, 44, 255), 2);
			d->AddLine(ImVec2(c.x, c.y + s * 0.04f), ImVec2(c.x, c.y - s * 0.2f), IM_COL32(90, 150, 60, 255), line);
			leaf(d, ImVec2(c.x, c.y - s * 0.12f), s * 0.24f, -PI * 0.85f, IM_COL32(100, 176, 64, 255));
			leaf(d, ImVec2(c.x, c.y - s * 0.18f), s * 0.26f, -PI * 0.2f, IM_COL32(120, 196, 76, 255));
			break;
		}
		case Picture::Open: {
			// an open book
			const ImU32 page = IM_COL32(246, 234, 204, 255);
			d->AddQuadFilled(ImVec2(c.x - s * 0.36f, c.y - s * 0.22f), ImVec2(c.x, c.y - s * 0.16f), ImVec2(c.x, c.y + s * 0.26f), ImVec2(c.x - s * 0.36f, c.y + s * 0.2f), page);
			d->AddQuadFilled(ImVec2(c.x, c.y - s * 0.16f), ImVec2(c.x + s * 0.36f, c.y - s * 0.22f), ImVec2(c.x + s * 0.36f, c.y + s * 0.2f), ImVec2(c.x, c.y + s * 0.26f), page);
			d->AddLine(ImVec2(c.x, c.y - s * 0.16f), ImVec2(c.x, c.y + s * 0.26f), IM_COL32(150, 110, 60, 255), 1.2f);
			for (float y : {-0.06f, 0.03f, 0.12f}) {
				d->AddLine(ImVec2(c.x - s * 0.29f, c.y + s * y), ImVec2(c.x - s * 0.07f, c.y + s * (y + 0.02f)), IM_COL32(150, 120, 90, 200), 1);
				d->AddLine(ImVec2(c.x + s * 0.07f, c.y + s * (y + 0.02f)), ImVec2(c.x + s * 0.29f, c.y + s * y), IM_COL32(150, 120, 90, 200), 1);
			}
			break;
		}
		case Picture::Save:
		case Picture::SaveAs: {
			// a 1996 floppy disk
			const ImVec2 a(c.x - s * 0.3f, c.y - s * 0.3f), b(c.x + s * 0.3f, c.y + s * 0.3f);
			d->AddRectFilled(a, b, IM_COL32(60, 90, 150, 255), 3);
			d->AddRectFilled(ImVec2(c.x - s * 0.16f, a.y), ImVec2(c.x + s * 0.16f, c.y - s * 0.1f), IM_COL32(200, 205, 215, 255));
			d->AddRectFilled(ImVec2(c.x + s * 0.04f, a.y + s * 0.04f), ImVec2(c.x + s * 0.11f, c.y - s * 0.14f), IM_COL32(60, 90, 150, 255));
			d->AddRectFilled(ImVec2(c.x - s * 0.2f, c.y + s * 0.03f), ImVec2(c.x + s * 0.2f, b.y), IM_COL32(244, 240, 228, 255), 2);
			if (p == Picture::SaveAs) {
				// a quill across it
				d->AddLine(ImVec2(c.x + s * 0.05f, c.y + s * 0.38f), ImVec2(c.x + s * 0.42f, c.y - s * 0.05f), IM_COL32(60, 40, 20, 255), line);
				leaf(d, ImVec2(c.x + s * 0.24f, c.y + s * 0.16f), s * 0.3f, -PI * 0.25f, IM_COL32(236, 230, 214, 255));
			}
			break;
		}
		case Picture::Import:
		case Picture::Export: {
			d->AddEllipseFilled(ImVec2(c.x - s * 0.06f, c.y + s * 0.06f), ImVec2(s * 0.2f, s * 0.27f), IM_COL32(244, 232, 206, 255), 0, 20);
			d->AddCircleFilled(ImVec2(c.x - s * 0.1f, c.y), s * 0.03f, IM_COL32(160, 120, 80, 255), 8);
			d->AddCircleFilled(ImVec2(c.x + s * 0.01f, c.y + s * 0.15f), s * 0.03f, IM_COL32(160, 120, 80, 255), 8);
			// the arrow: into the egg from the top right, or out of it
			const ImVec2 tail = p == Picture::Import ? ImVec2(c.x + s * 0.38f, c.y - s * 0.34f) : ImVec2(c.x + s * 0.08f, c.y - s * 0.06f);
			const ImVec2 head = p == Picture::Import ? ImVec2(c.x + s * 0.1f, c.y - s * 0.08f) : ImVec2(c.x + s * 0.38f, c.y - s * 0.34f);
			d->AddLine(tail, head, IM_COL32(110, 200, 90, 255), line * 1.3f);
			const ImVec2 dir((head.x - tail.x) / s, (head.y - tail.y) / s);
			const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
			const ImVec2 u(dir.x / len, dir.y / len), n(-u.y, u.x);
			d->AddTriangleFilled(ImVec2(head.x + u.x * s * 0.08f, head.y + u.y * s * 0.08f), ImVec2(head.x - u.x * s * 0.06f + n.x * s * 0.09f, head.y - u.y * s * 0.06f + n.y * s * 0.09f),
				ImVec2(head.x - u.x * s * 0.06f - n.x * s * 0.09f, head.y - u.y * s * 0.06f - n.y * s * 0.09f), IM_COL32(110, 200, 90, 255));
			break;
		}
		case Picture::Settings: {
			for (int i = 0; i < 8; ++i) {
				const float a = static_cast<float>(i) * PI / 4;
				d->AddLine(polar(c, s * 0.2f, a), polar(c, s * 0.36f, a), metal, line * 1.9f);
			}
			d->AddCircleFilled(c, s * 0.26f, metal, 24);
			d->AddCircleFilled(c, s * 0.1f, IM_COL32(30, 36, 40, 255), 16);
			break;
		}
		case Picture::Help: {
			ImFont* font = ImGui::GetFont();
			const float size = s * 0.85f;
			const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, "?");
			d->AddText(font, size, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), metal, "?");
			break;
		}
		case Picture::Quit: {
			// a wooden door, ajar
			d->AddRectFilled(ImVec2(c.x - s * 0.24f, c.y - s * 0.34f), ImVec2(c.x + s * 0.2f, c.y + s * 0.34f), IM_COL32(40, 30, 22, 255), 3);
			d->AddQuadFilled(ImVec2(c.x - s * 0.24f, c.y - s * 0.34f), ImVec2(c.x + s * 0.08f, c.y - s * 0.26f), ImVec2(c.x + s * 0.08f, c.y + s * 0.4f), ImVec2(c.x - s * 0.24f, c.y + s * 0.34f), IM_COL32(170, 112, 60, 255));
			d->AddCircleFilled(ImVec2(c.x + s * 0.01f, c.y + s * 0.04f), s * 0.035f, metal, 8);
			break;
		}
		case Picture::Tree: {
			// a little family tree: one at the top, two below, joined
			const ImU32 node = IM_COL32(236, 196, 120, 255), branch = IM_COL32(140, 200, 100, 255);
			d->AddLine(ImVec2(c.x, c.y - s * 0.16f), ImVec2(c.x - s * 0.2f, c.y + s * 0.16f), branch, line);
			d->AddLine(ImVec2(c.x, c.y - s * 0.16f), ImVec2(c.x + s * 0.2f, c.y + s * 0.16f), branch, line);
			d->AddCircleFilled(ImVec2(c.x, c.y - s * 0.2f), s * 0.11f, node, 14);
			d->AddCircleFilled(ImVec2(c.x - s * 0.22f, c.y + s * 0.2f), s * 0.1f, IM_COL32(236, 120, 170, 255), 14);
			d->AddCircleFilled(ImVec2(c.x + s * 0.22f, c.y + s * 0.2f), s * 0.1f, IM_COL32(100, 160, 240, 255), 14);
			break;
		}
		case Picture::Paw: {
			d->AddEllipseFilled(ImVec2(c.x, c.y + s * 0.1f), ImVec2(s * 0.17f, s * 0.14f), metal, 0, 18);
			for (int i = 0; i < 4; ++i) {
				const float a = -PI * 0.5f + (static_cast<float>(i) - 1.5f) * 0.55f;
				d->AddCircleFilled(polar(ImVec2(c.x, c.y + s * 0.06f), s * 0.28f, a), s * 0.075f, metal, 12);
			}
			break;
		}
		case Picture::ToStart:
		case Picture::StepBack:
		case Picture::Backwards:
		case Picture::Play:
		case Picture::Pause:
		case Picture::StepOn:
		case Picture::ToEnd:
			controls(d, p, c, s, metal);
			break;
		case Picture::Record:
			d->AddCircle(c, s * 0.3f, metal, 24, line);
			d->AddCircleFilled(c, s * (lit ? 0.2f : 0.17f), lit ? IM_COL32(255, 70, 50, 255) : IM_COL32(200, 60, 45, 255), 20);
			break;
		case Picture::Menu:
			glyph(d, Glyph::Menu, c, s * 0.8f, metal);
			break;
		case Picture::Slap:
			hand(d, ImVec2(c.x - s * 0.08f, c.y + s * 0.04f), s, IM_COL32(246, 214, 170, 255));
			// a smack: little lines
			for (int i = 0; i < 3; ++i) {
				const float a = -PI * 0.5f + static_cast<float>(i - 1) * 0.5f;
				d->AddLine(polar(ImVec2(c.x + s * 0.24f, c.y - s * 0.14f), s * 0.06f, a), polar(ImVec2(c.x + s * 0.24f, c.y - s * 0.14f), s * 0.18f, a), IM_COL32(230, 80, 50, 255), line);
			}
			break;
	}
}

void controls(ImDrawList* d, Picture p, ImVec2 c, float s, ImU32 colour) {
	// triangles pointing right (dir 1) or left (-1), their points at x
	auto triangle = [&](float x, float dir) {
		const float w = s * 0.26f, h = s * 0.25f;
		d->AddTriangleFilled(ImVec2(x, c.y), ImVec2(x - dir * w, c.y - h), ImVec2(x - dir * w, c.y + h), colour);
	};
	auto bar = [&](float x) { d->AddRectFilled(ImVec2(x - s * 0.04f, c.y - s * 0.25f), ImVec2(x + s * 0.04f, c.y + s * 0.25f), colour, s * 0.02f); };
	switch (p) {
		case Picture::Play: d->AddTriangleFilled(ImVec2(c.x + s * 0.2f, c.y), ImVec2(c.x - s * 0.14f, c.y - s * 0.3f), ImVec2(c.x - s * 0.14f, c.y + s * 0.3f), colour); break;
		case Picture::Pause:
			d->AddRectFilled(ImVec2(c.x - s * 0.17f, c.y - s * 0.26f), ImVec2(c.x - s * 0.05f, c.y + s * 0.26f), colour, s * 0.03f);
			d->AddRectFilled(ImVec2(c.x + s * 0.05f, c.y - s * 0.26f), ImVec2(c.x + s * 0.17f, c.y + s * 0.26f), colour, s * 0.03f);
			break;
		case Picture::Backwards: triangle(c.x - s * 0.24f, -1); triangle(c.x + s * 0.02f, -1); break;
		case Picture::StepBack: triangle(c.x - s * 0.16f, -1); bar(c.x + s * 0.16f); break;
		case Picture::StepOn: bar(c.x - s * 0.16f); triangle(c.x + s * 0.16f, 1); break;
		case Picture::ToStart: bar(c.x - s * 0.3f); triangle(c.x - s * 0.22f, -1); triangle(c.x + s * 0.04f, -1); break;
		case Picture::ToEnd: triangle(c.x - s * 0.04f, 1); triangle(c.x + s * 0.22f, 1); bar(c.x + s * 0.3f); break;
		default: break;
	}
}

// --- the glyphs ---

namespace {

// a line with an arrowhead at to
void arrow(ImDrawList* d, ImVec2 from, ImVec2 to, ImU32 colour, float thickness, float head) {
	const float dx = to.x - from.x, dy = to.y - from.y, len = std::max(0.001f, std::sqrt(dx * dx + dy * dy));
	const ImVec2 u(dx / len, dy / len), n(-u.y, u.x);
	d->AddLine(from, ImVec2(to.x - u.x * head * 0.6f, to.y - u.y * head * 0.6f), colour, thickness);
	d->AddTriangleFilled(to, ImVec2(to.x - u.x * head + n.x * head * 0.6f, to.y - u.y * head + n.y * head * 0.6f),
		ImVec2(to.x - u.x * head - n.x * head * 0.6f, to.y - u.y * head - n.y * head * 0.6f), colour);
}

// a star: points of radius outer, and inner between them, the first pointing up
void star(ImDrawList* d, ImVec2 c, int points, float outer, float inner, ImU32 colour) {
	d->PathClear();
	for (int i = 0; i < points * 2; ++i) {
		d->PathLineTo(polar(c, i % 2 ? inner : outer, -PI / 2 + static_cast<float>(i) * PI / static_cast<float>(points)));
	}
	d->PathFillConcave(colour);
}

// the part of circle 0 outside circle 1 (a crescent), the circles overlapping
void crescent(ImDrawList* d, ImVec2 c0, float r0, ImVec2 c1, float r1, ImU32 colour) {
	const float dx = c1.x - c0.x, dy = c1.y - c0.y, dist = std::sqrt(dx * dx + dy * dy);
	const float a = (r0 * r0 - r1 * r1 + dist * dist) / (2 * dist);
	const float h = std::sqrt(std::max(0.f, r0 * r0 - a * a));
	const ImVec2 mid(c0.x + a * dx / dist, c0.y + a * dy / dist);
	const ImVec2 p(mid.x + h * dy / dist, mid.y - h * dx / dist), q(mid.x - h * dy / dist, mid.y + h * dx / dist);
	const float p0 = std::atan2(p.y - c0.y, p.x - c0.x), q0 = std::atan2(q.y - c0.y, q.x - c0.x);
	const float p1 = std::atan2(p.y - c1.y, p.x - c1.x), q1 = std::atan2(q.y - c1.y, q.x - c1.x);
	// round circle 0 the long way, from q to p, then back inside circle 1 from p to q
	float from0 = q0, to0 = p0;
	while (to0 < from0) {
		to0 += 2 * PI;
	}
	float from1 = p1, to1 = q1;
	while (to1 > from1) {
		to1 -= 2 * PI;
	}
	d->PathClear();
	d->PathArcTo(c0, r0, from0, to0, 24);
	d->PathArcTo(c1, r1, from1, to1, 20);
	d->PathFillConcave(colour);
}

void person(ImDrawList* d, ImVec2 c, float s, ImU32 colour) {
	d->AddCircleFilled(ImVec2(c.x, c.y - s * 0.18f), s * 0.15f, colour, 16);
	d->PathClear();
	d->PathArcTo(ImVec2(c.x, c.y + s * 0.4f), s * 0.3f, PI, 2 * PI, 16);
	d->PathFillConvex(colour);
}

void zed(ImDrawList* d, ImVec2 c, float s, ImU32 colour, float thickness) {
	const ImVec2 pts[] = {ImVec2(c.x - s / 2, c.y - s / 2), ImVec2(c.x + s / 2, c.y - s / 2), ImVec2(c.x - s / 2, c.y + s / 2), ImVec2(c.x + s / 2, c.y + s / 2)};
	d->AddPolyline(pts, 4, colour, 0, thickness);
}

} // namespace

void glyph(ImDrawList* d, Glyph g, ImVec2 c, float s, ImU32 colour) {
	const float lw = std::max(1.3f, s * 0.1f);
	auto at = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
	switch (g) {
		case Glyph::Pain: // a burst
			star(d, c, 8, s * 0.46f, s * 0.22f, colour);
			break;
		case Glyph::Pleasure: // a sparkle and a little one
			star(d, at(-0.06f, 0.06f), 4, s * 0.4f, s * 0.11f, colour);
			star(d, at(0.28f, -0.28f), 4, s * 0.16f, s * 0.05f, colour);
			break;
		case Glyph::Hunger: // an apple
			d->AddCircleFilled(at(-0.1f, 0.08f), s * 0.24f, colour, 20);
			d->AddCircleFilled(at(0.1f, 0.08f), s * 0.24f, colour, 20);
			d->AddLine(at(0, -0.14f), at(0.05f, -0.38f), colour, lw);
			d->AddEllipseFilled(at(0.18f, -0.3f), ImVec2(s * 0.13f, s * 0.06f), colour, -0.5f, 12);
			break;
		case Glyph::Cold: // a snowflake
			for (int i = 0; i < 3; ++i) {
				const float a = PI / 2 + static_cast<float>(i) * PI / 3;
				d->AddLine(polar(c, s * 0.44f, a), polar(c, s * 0.44f, a + PI), colour, lw);
				for (float end : {0.f, PI}) {
					const ImVec2 tip = polar(c, s * 0.3f, a + end);
					d->AddLine(tip, polar(tip, s * 0.13f, a + end + PI / 4), colour, lw * 0.8f);
					d->AddLine(tip, polar(tip, s * 0.13f, a + end - PI / 4), colour, lw * 0.8f);
				}
			}
			break;
		case Glyph::Hot: // the sun
			d->AddCircleFilled(c, s * 0.2f, colour, 20);
			for (int i = 0; i < 8; ++i) {
				const float a = static_cast<float>(i) * PI / 4;
				d->AddLine(polar(c, s * 0.3f, a), polar(c, s * 0.45f, a), colour, lw);
			}
			break;
		case Glyph::Tired: // a battery nearly flat
			d->AddRect(at(-0.38f, -0.2f), at(0.28f, 0.2f), colour, s * 0.06f, 0, lw);
			d->AddRectFilled(at(0.28f, -0.09f), at(0.4f, 0.09f), colour, s * 0.03f);
			d->AddRectFilled(at(-0.28f, -0.1f), at(-0.12f, 0.1f), colour, s * 0.02f);
			break;
		case Glyph::Sleepy: // the moon and a little z
			crescent(d, at(-0.06f, 0.06f), s * 0.36f, at(0.14f, -0.08f), s * 0.3f, colour);
			zed(d, at(0.3f, -0.3f), s * 0.18f, colour, lw * 0.75f);
			break;
		case Glyph::Lonely:
			person(d, at(0, 0.02f), s, colour);
			break;
		case Glyph::Crowded:
			person(d, at(-0.26f, 0.08f), s * 0.7f, colour);
			person(d, at(0.26f, 0.08f), s * 0.7f, colour);
			person(d, at(0, 0), s * 0.85f, colour);
			break;
		case Glyph::Fear: { // a warning
			const ImVec2 pts[] = {at(0, -0.4f), at(0.43f, 0.36f), at(-0.43f, 0.36f)};
			d->AddPolyline(pts, 3, colour, ImDrawFlags_Closed, lw);
			d->AddLine(at(0, -0.14f), at(0, 0.1f), colour, lw * 1.1f);
			d->AddCircleFilled(at(0, 0.22f), lw * 0.65f, colour, 8);
			break;
		}
		case Glyph::Boredom: // an hourglass
			d->AddLine(at(-0.3f, -0.4f), at(0.3f, -0.4f), colour, lw);
			d->AddLine(at(-0.3f, 0.4f), at(0.3f, 0.4f), colour, lw);
			d->AddTriangle(at(-0.24f, -0.36f), at(0.24f, -0.36f), at(0, 0), colour, lw * 0.8f);
			d->AddTriangle(at(-0.24f, 0.36f), at(0.24f, 0.36f), at(0, 0), colour, lw * 0.8f);
			d->AddTriangleFilled(at(-0.15f, 0.34f), at(0.15f, 0.34f), at(0, 0.14f), colour);
			break;
		case Glyph::Anger: { // lightning
			const ImVec2 pts[] = {at(0.12f, -0.46f), at(-0.26f, 0.06f), at(-0.02f, 0.06f), at(-0.14f, 0.46f), at(0.26f, -0.08f), at(0.03f, -0.08f)};
			d->AddConcavePolyFilled(pts, 6, colour);
			break;
		}
		case Glyph::SexDrive:
		case Glyph::Heart:
			d->AddCircleFilled(at(-0.13f, -0.1f), s * 0.18f, colour, 16);
			d->AddCircleFilled(at(0.13f, -0.1f), s * 0.18f, colour, 16);
			d->AddTriangleFilled(at(-0.3f, -0.03f), at(0.3f, -0.03f), at(0, 0.37f), colour);
			break;
		case Glyph::Push: // an arrow into a box
			d->AddRect(at(0.08f, -0.22f), at(0.44f, 0.22f), colour, s * 0.04f, 0, lw);
			arrow(d, at(-0.44f, 0), at(0.02f, 0), colour, lw, s * 0.22f);
			break;
		case Glyph::Pull: // an arrow away from a box
			d->AddRect(at(0.08f, -0.22f), at(0.44f, 0.22f), colour, s * 0.04f, 0, lw);
			arrow(d, at(0.02f, 0), at(-0.44f, 0), colour, lw, s * 0.22f);
			break;
		case Glyph::Stop: { // a stop sign
			ImVec2 pts[8];
			for (int i = 0; i < 8; ++i) {
				pts[i] = polar(c, s * 0.44f, PI / 8 + static_cast<float>(i) * PI / 4);
			}
			d->AddConvexPolyFilled(pts, 8, colour);
			break;
		}
		case Glyph::Come: // an arrow coming to someone
			person(d, at(-0.16f, 0.06f), s * 0.8f, colour);
			arrow(d, at(0.44f, -0.4f), at(0.12f, -0.08f), colour, lw, s * 0.2f);
			break;
		case Glyph::Run: // two chevrons, fast
			for (float x : {-0.3f, 0.04f}) {
				const ImVec2 pts[] = {at(x - 0.08f, -0.3f), at(x + 0.2f, 0), at(x - 0.08f, 0.3f)};
				d->AddPolyline(pts, 3, colour, 0, lw * 1.2f);
			}
			break;
		case Glyph::Get: { // a ball into a hand
			d->AddCircleFilled(at(0, -0.22f), s * 0.15f, colour, 16);
			const ImVec2 pts[] = {at(-0.36f, 0.02f), at(-0.3f, 0.36f), at(0.3f, 0.36f), at(0.36f, 0.02f)};
			d->AddPolyline(pts, 4, colour, 0, lw);
			break;
		}
		case Glyph::Drop: // down, onto the ground
			arrow(d, at(0, -0.44f), at(0, 0.2f), colour, lw, s * 0.24f);
			d->AddLine(at(-0.36f, 0.38f), at(0.36f, 0.38f), colour, lw);
			break;
		case Glyph::Rest:
			zed(d, at(-0.08f, 0.1f), s * 0.42f, colour, lw);
			zed(d, at(0.28f, -0.26f), s * 0.22f, colour, lw * 0.8f);
			break;
		case Glyph::West:
			arrow(d, at(0.42f, 0), at(-0.42f, 0), colour, lw * 1.1f, s * 0.3f);
			break;
		case Glyph::East:
			arrow(d, at(-0.42f, 0), at(0.42f, 0), colour, lw * 1.1f, s * 0.3f);
			break;
		case Glyph::Stay: // an anchor
			d->AddCircle(at(0, -0.33f), s * 0.09f, colour, 12, lw * 0.8f);
			d->AddLine(at(0, -0.24f), at(0, 0.4f), colour, lw);
			d->AddLine(at(-0.18f, -0.12f), at(0.18f, -0.12f), colour, lw);
			arc(d, at(0, 0.04f), s * 0.34f, PI * 0.12f, PI * 0.88f, colour, lw);
			break;
		case Glyph::Speak: { // a speech bubble
			d->AddRect(at(-0.42f, -0.32f), at(0.42f, 0.16f), colour, s * 0.14f, 0, lw);
			const ImVec2 tail[] = {at(-0.22f, 0.16f), at(-0.3f, 0.4f), at(-0.02f, 0.16f)};
			d->AddPolyline(tail, 3, colour, 0, lw);
			for (float x : {-0.18f, 0.f, 0.18f}) {
				d->AddCircleFilled(at(x, -0.08f), lw * 0.6f, colour, 8);
			}
			break;
		}
		case Glyph::Why: {
			ImFont* font = ImGui::GetFont();
			const float size = s * 1.05f;
			const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, "?");
			d->AddText(font, size, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), colour, "?");
			d->AddText(font, size, ImVec2(c.x - ts.x / 2 + 0.7f, c.y - ts.y / 2), colour, "?");
			break;
		}
		case Glyph::Star:
			star(d, c, 5, s * 0.48f, s * 0.2f, colour);
			break;
		case Glyph::Eye:
			d->PathClear();
			d->PathArcTo(at(0, 0.34f), s * 0.56f, -PI * 0.78f, -PI * 0.22f, 16);
			d->PathArcTo(at(0, -0.34f), s * 0.56f, PI * 0.22f, PI * 0.78f, 16);
			d->PathStroke(colour, ImDrawFlags_Closed, lw);
			d->AddCircleFilled(c, s * 0.13f, colour, 12);
			break;
		case Glyph::Tag: {
			const ImVec2 pts[] = {at(-0.44f, 0), at(-0.2f, -0.24f), at(0.42f, -0.24f), at(0.42f, 0.24f), at(-0.2f, 0.24f)};
			d->AddPolyline(pts, 5, colour, ImDrawFlags_Closed, lw);
			d->AddCircleFilled(at(-0.16f, 0), lw * 0.7f, colour, 8);
			break;
		}
		case Glyph::Check: {
			const ImVec2 pts[] = {at(-0.34f, 0.02f), at(-0.1f, 0.26f), at(0.36f, -0.26f)};
			d->AddPolyline(pts, 3, colour, 0, lw * 1.3f);
			break;
		}
		case Glyph::Pill: // a capsule, its halves two shades
			d->AddLine(at(-0.2f, 0.2f), at(0, 0), scale_alpha(colour, 0.55f), s * 0.3f);
			d->AddCircleFilled(at(-0.2f, 0.2f), s * 0.15f, scale_alpha(colour, 0.55f), 14);
			d->AddLine(at(0, 0), at(0.2f, -0.2f), colour, s * 0.3f);
			d->AddCircleFilled(at(0.2f, -0.2f), s * 0.15f, colour, 14);
			break;
		case Glyph::Chevron: {
			const ImVec2 pts[] = {at(-0.12f, -0.26f), at(0.14f, 0), at(-0.12f, 0.26f)};
			d->AddPolyline(pts, 3, colour, 0, lw * 1.2f);
			break;
		}
		case Glyph::Dead: // a headstone
			d->PathClear();
			d->PathArcTo(at(0, -0.08f), s * 0.28f, PI, 2 * PI, 14);
			d->PathLineTo(at(0.28f, 0.38f));
			d->PathLineTo(at(-0.28f, 0.38f));
			d->PathStroke(colour, ImDrawFlags_Closed, lw);
			d->AddLine(at(0, -0.2f), at(0, 0.2f), colour, lw * 0.8f);
			d->AddLine(at(-0.11f, -0.06f), at(0.11f, -0.06f), colour, lw * 0.8f);
			break;
		case Glyph::Male:
			d->AddCircle(at(-0.1f, 0.1f), s * 0.22f, colour, 16, lw);
			arrow(d, at(0.06f, -0.06f), at(0.42f, -0.42f), colour, lw, s * 0.22f);
			break;
		case Glyph::Female:
			d->AddCircle(at(0, -0.12f), s * 0.22f, colour, 16, lw);
			d->AddLine(at(0, 0.1f), at(0, 0.46f), colour, lw);
			d->AddLine(at(-0.15f, 0.3f), at(0.15f, 0.3f), colour, lw);
			break;
		case Glyph::Menu:
			for (float y : {-0.26f, 0.f, 0.26f}) {
				d->AddLine(at(-0.34f, y), at(0.34f, y), colour, lw * 1.2f);
			}
			break;
		case Glyph::Dot:
			d->AddCircleFilled(c, s * 0.14f, colour, 12);
			break;
	}
}

Glyph drive_glyph(size_t drive) {
	static const Glyph drives[] = {Glyph::Pain, Glyph::Pleasure, Glyph::Hunger, Glyph::Cold, Glyph::Hot, Glyph::Tired, Glyph::Sleepy,
		Glyph::Lonely, Glyph::Crowded, Glyph::Fear, Glyph::Boredom, Glyph::Anger, Glyph::SexDrive};
	return drive < std::size(drives) ? drives[drive] : Glyph::Dot;
}

float chip_width(const char* label) {
	const float h = ImGui::GetFrameHeight(), icon = h * 0.62f, pad = h * 0.34f;
	const float tw = ImGui::CalcTextSize(label).x;
	return pad + icon + (tw > 0 ? pad * 0.6f + tw : 0) + pad;
}

bool chip(const char* id, Glyph g, const char* label, ImU32 face, ImU32 ink, float width) {
	const float h = ImGui::GetFrameHeight(), icon = h * 0.62f, pad = h * 0.34f;
	const ImVec2 ts = ImGui::CalcTextSize(label);
	const float w = width > 0 ? width : chip_width(label);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
	const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
	auto* d = ImGui::GetWindowDrawList();
	const ImU32 base = face ? face : IM_COL32(72, 50, 32, 255);
	d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), held ? BRASS : hovered ? IM_COL32(110, 78, 46, 255) : base, h / 2);
	d->AddRect(p, ImVec2(p.x + w, p.y + h), hovered ? BRASS_LIGHT : scale_alpha(BRASS, 0.6f), h / 2, 0, 1);
	const ImU32 colour = held ? IM_COL32(56, 34, 10, 255) : ink;
	glyph(d, g, ImVec2(p.x + pad + icon / 2, p.y + h / 2), icon, colour);
	if (ts.x > 0) {
		d->AddText(ImVec2(p.x + pad + icon + pad * 0.6f, p.y + (h - ts.y) / 2), colour, label);
	}
	return pressed;
}

void glyph_text(Glyph g, ImU32 glyph_colour, const char* text, ImU32 text_colour) {
	const float h = ImGui::GetTextLineHeight();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(h, h));
	glyph(ImGui::GetWindowDrawList(), g, ImVec2(p.x + h / 2, p.y + h / 2), h * 0.9f, glyph_colour);
	ImGui::SameLine(0, h * 0.3f);
	if (text_colour) {
		ImGui::PushStyleColor(ImGuiCol_Text, text_colour);
	}
	ImGui::TextUnformatted(text);
	if (text_colour) {
		ImGui::PopStyleColor();
	}
}

float ease(unsigned key, float target, float speed) {
	static std::unordered_map<unsigned, float> values;
	auto [it, fresh] = values.emplace(key, target);
	if (!fresh) {
		const float dt = std::min(0.1f, ImGui::GetIO().DeltaTime);
		it->second += (target - it->second) * (1 - std::exp(-dt * speed));
		if (std::abs(target - it->second) < 0.002f) {
			it->second = target;
		}
	}
	return it->second;
}

bool round_button(ImDrawList* d, const char* id, ImVec2 c, float base_r, Picture p, const char* tooltip, bool on, bool enabled) {
	ImGui::SetCursorScreenPos(ImVec2(c.x - base_r, c.y - base_r));
	ImGui::BeginDisabled(!enabled);
	const bool pressed = ImGui::InvisibleButton(id, ImVec2(base_r * 2, base_r * 2)) && enabled;
	const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
	const bool held = ImGui::IsItemActive();
	const unsigned key = ImGui::GetItemID();
	if (tooltip && tooltip[0]) {
		ImGui::SetItemTooltip("%s", tooltip);
	}
	ImGui::EndDisabled();
	// it swells a little under the pointer, dips when pressed, and glows in and out
	const float lift = ease(key, hovered && enabled ? 1.f : 0.f);
	const float press = ease(key ^ 0x9e3779b9u, held ? 1.f : 0.f, 30);
	const float glow = ease(key ^ 0x7f4a7c15u, on ? 1.f : 0.f, 10);
	const float r = base_r * (1 + 0.07f * lift - 0.05f * press);
	const ImVec2 at(c.x, c.y - 1.5f * lift + 1.5f * press);
	shadow_circle(d, c, r);
	brass_ring(d, at, r, r * 0.8f);
	glass(d, at, r * 0.8f);
	if (glow > 0.01f) {
		d->AddCircleFilled(at, r * 0.8f, IM_COL32(255, 188, 80, static_cast<int>(70 * glow)), 32);
		d->AddCircle(at, r * 0.74f, IM_COL32(255, 210, 120, static_cast<int>(200 * glow)), 32, 2);
	}
	if (lift > 0.01f) {
		d->AddCircleFilled(at, r * 0.8f, IM_COL32(255, 240, 200, static_cast<int>(28 * lift)), 32);
		d->AddCircle(at, r + 1, IM_COL32(255, 236, 170, static_cast<int>(200 * lift)), 32, 1.5f);
	}
	ImDrawList* target = d;
	const int before = target->VtxBuffer.Size;
	picture(target, p, at, r * 1.32f, hovered || on);
	if (!enabled) {
		// fade the picture
		for (int i = before; i < target->VtxBuffer.Size; ++i) {
			ImU32& col = target->VtxBuffer[i].col;
			col = scale_alpha(col, 0.35f);
		}
	}
	glass_shine(d, at, r * 0.8f);
	return pressed;
}

void window_frame(ImDrawList* d, ImVec2 a, ImVec2 b, float border, const char* title, bool paper) {
	wood_panel(d, a, b, 14);
	// the inside, sunk below the frame: dark, or a page of warm paper (the kits' panes)
	const ImVec2 ia(a.x + border, a.y + border), ib(b.x - border, b.y - border);
	if (paper) {
		d->AddRectFilledMultiColor(ia, ib, IM_COL32(244, 234, 208, 255), IM_COL32(242, 230, 202, 255), IM_COL32(230, 214, 180, 255), IM_COL32(234, 219, 186, 255));
		// the page's edges a little darker, as old paper is
		for (int i = 0; i < 6; ++i) {
			const float f = static_cast<float>(i);
			d->AddRect(ImVec2(ia.x + f, ia.y + f), ImVec2(ib.x - f, ib.y - f), IM_COL32(150, 110, 60, 26 - 4 * i), 8, 0, 1.5f);
		}
	} else {
		d->AddRectFilled(ia, ib, IM_COL32(30, 22, 16, 246), 8);
	}
	d->AddRect(ia, ib, IM_COL32(10, 6, 2, 220), 8, 0, 2);
	d->AddRect(ImVec2(ia.x - 1, ia.y - 1), ImVec2(ib.x + 1, ib.y + 1), IM_COL32(255, 220, 160, 40), 9, 0, 1);
	for (const ImVec2 corner : {ImVec2(a.x + border * 0.5f, a.y + border * 0.5f), ImVec2(b.x - border * 0.5f, a.y + border * 0.5f),
			 ImVec2(a.x + border * 0.5f, b.y - border * 0.5f), ImVec2(b.x - border * 0.5f, b.y - border * 0.5f)}) {
		rivet(d, corner, std::max(2.5f, border * 0.22f));
	}
	if (title && title[0]) {
		ImFont* font = ImGui::GetFont();
		const float size = ImGui::GetFontSize() * 1.1f;
		const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, title);
		const float w = ts.x + size * 2.4f, h = std::max(border * 1.5f, ts.y + 8);
		const ImVec2 pa((a.x + b.x - w) / 2, a.y + border / 2 - h / 2), pb((a.x + b.x + w) / 2, a.y + border / 2 + h / 2);
		brass_plate(d, pa, pb, h * 0.3f);
		d->AddText(font, size, ImVec2((a.x + b.x - ts.x) / 2, (pa.y + pb.y - ts.y) / 2), IM_COL32(56, 34, 10, 255), title);
	}
}

} // namespace UIArt
