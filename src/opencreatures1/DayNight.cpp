#include "DayNight.h"

#include "EngineContext.h"
#include "GameUI.h"
#include "ImageManager.h"
#include "MapManager.h"
#include "PathManager.h"
#include "PointerManager.h"
#include "UIArt.h"
#include "ViewportManager.h"
#include "WorldState.h"
#include "common/io/FileReader.h"
#include "fileformats/sprImage.h"
#include "objects/Object.h"
#include "objects/ObjectManager.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fmt/core.h>
#include <functional>
#include <map>
#include <random>
#include <vector>

namespace DayNight {

namespace {

constexpr float PI = 3.14159265f;
// the light is worked out for cells of 16 pixels, and smoothed when drawn
constexpr int CELL = 16;
constexpr int COLS = CREATURES1_WORLD_WIDTH / CELL;
constexpr int ROWS = CREATURES1_WORLD_HEIGHT / CELL;
// the shipped world's sea is the blue below this height (the sky above it is blue too)
constexpr int SEA_LEVEL = 600;
// the sky, for the stars, is looked at in cells of 4 pixels down to here
constexpr int SKY_CELL = 4;
constexpr int SKY_COLS = CREATURES1_WORLD_WIDTH / SKY_CELL;
constexpr int SKY_ROWS = 620 / SKY_CELL;
// the light drawn into the light map is half strength there, so the world can be lit up to
// twice as bright as its pictures (a light map of 0.5 leaves it as it is)
constexpr float HALF = 0.5f;

// What a cell is, for the light: open air, a building, a room underground, the ground (or a
// wall), or the sea.
enum Kind : uint8_t { OPEN, INDOOR, UNDERGROUND, SOLID, WATER };

// a lamp in the background, in its own colour
struct Lamp {
	float x, y, phase, r, g, b;
};
struct Star {
	float x, y, size, bright, phase, speed, r, g, b;
};
// a firefly's home, and how it wanders and blinks
struct Firefly {
	float x, y, p1, p2, p3, p4, period, offset;
};
// where a shaft of light comes down through the sea
struct Ray {
	float x, top, depth, seed;
};
struct Box {
	float x, y, w, h;
};

struct WorldLook {
	uint64_t generation = ~0ull;
	std::vector<Kind> kind = std::vector<Kind>(static_cast<size_t>(COLS * ROWS), OPEN);
	// how strongly the water ripples with light (open to the sky, less with depth), and
	// where the sea is open to the sky (for the shafts)
	std::vector<float> wet = std::vector<float>(static_cast<size_t>(COLS * ROWS), 0.f);
	std::vector<float> sea = std::vector<float>(static_cast<size_t>(COLS * ROWS), 0.f);
	std::vector<uint8_t> sky = std::vector<uint8_t>(static_cast<size_t>(SKY_COLS * SKY_ROWS), 0);
	std::vector<Lamp> lamps;
	std::vector<Star> stars;
	std::vector<Firefly> fireflies;
	std::vector<Ray> rays;
	std::vector<Box> rooms; // the rooms of buildings, warmly lit
} s_world;

struct Light {
	std::vector<uint8_t> rgba = std::vector<uint8_t>(static_cast<size_t>(COLS * ROWS * 4), 255);
	float made_for = -1;
	SDL_Renderer* renderer = nullptr;
	SDL_Texture* texture = nullptr; // the light of each cell
	SDL_Texture* glow = nullptr; // a soft round glow
	SDL_Texture* sparkle = nullptr; // a tiny four-pointed star
	SDL_Texture* ripples = nullptr; // light rippling under water, tiling
	SDL_Texture* map = nullptr; // the light map for the view, drawn each frame
	int map_w = 0, map_h = 0;
	SDL_BlendMode lit = SDL_BLENDMODE_INVALID; // the world times twice the light map
	bool can_light = false;
} s_light;

struct ShootingStar {
	float x = 0, y = 0, vx = 0, vy = 0, age = 0, life = 0, tail = 0;
	bool alive = false;
	double next = 0;
	double last = 0;
} s_shooting;

std::mt19937 s_random{1996};
const Shown* s_playback = nullptr;

// what glows: the eggs (with a number each for their sparkles) and the hand
struct Glowing {
	std::vector<Rect2f> eggs, hands;
	std::vector<uint32_t> seeds;
};

float frand() {
	return std::uniform_real_distribution<float>(0.f, 1.f)(s_random);
}

// a number from 0 to 1 that depends only on its arguments
float hash(uint32_t a, uint32_t b = 0) {
	uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	h *= 0x297A2D39u;
	h ^= h >> 15;
	return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0x1000000);
}

Kind kind_at(int col, int row) {
	col = ((col % COLS) + COLS) % COLS;
	row = std::clamp(row, 0, ROWS - 1);
	return s_world.kind[static_cast<size_t>(row * COLS + col)];
}

// a cell map's value at a world point, smoothed between the cells' centres
float sample(const std::vector<float>& cells, float x, float y) {
	const float fx = x / CELL - 0.5f, fy = std::clamp(y / CELL - 0.5f, 0.f, static_cast<float>(ROWS - 1));
	const int c0 = static_cast<int>(std::floor(fx)), r0 = static_cast<int>(fy);
	const float ax = fx - static_cast<float>(c0), ay = fy - static_cast<float>(r0);
	auto at = [&](int c, int r) {
		c = ((c % COLS) + COLS) % COLS;
		r = std::min(r, ROWS - 1);
		return cells[static_cast<size_t>(r * COLS + c)];
	};
	return (at(c0, r0) * (1 - ax) + at(c0 + 1, r0) * ax) * (1 - ay) + (at(c0, r0 + 1) * (1 - ax) + at(c0 + 1, r0 + 1) * ax) * ay;
}

bool sky_at(float x, float y) {
	const int c = ((static_cast<int>(x) / SKY_CELL) % SKY_COLS + SKY_COLS) % SKY_COLS, r = static_cast<int>(y) / SKY_CELL;
	return y >= 0 && r < SKY_ROWS && s_world.sky[static_cast<size_t>(r * SKY_COLS + c)];
}

// The sun: the direction towards it (y down), how much daylight there is (with a twilight
// either side of the day), whether it's above the horizon, how golden its light is, and its
// colour. It's up from 0.25 to 0.75, rising on the left, highest at noon, setting on the right.
struct Sun {
	float dx = 0, dy = 0, strength = 0, warm = 0, r = 1, g = 1, b = 1;
	bool up = false;
};

float smoothstep(float from, float to, float x) {
	const float t = std::clamp((x - from) / (to - from), 0.f, 1.f);
	return t * t * (3 - 2 * t);
}

float lerp(float a, float b, float t) {
	return a + (b - a) * t;
}

Sun sun_at(float t) {
	Sun s;
	float day = (t - 0.25f) / 0.5f;
	if (day > 1.5f) {
		day -= 2;
	}
	const float a = PI * (1 - day);
	s.dx = std::cos(a);
	s.dy = -std::sin(a);
	const float height = std::sin(a); // below 0 under the horizon
	s.up = height > 0;
	s.strength = smoothstep(-0.14f, 0.38f, height);
	// golden when low, white high up
	s.warm = s.strength > 0 ? 1 - smoothstep(0.02f, 0.5f, height) : 0;
	s.g = 1 - 0.34f * s.warm;
	s.b = 1 - 0.62f * s.warm;
	return s;
}

// The lamps in the background: small bright spots, in any colour, with darkness all round
// them in that colour (a highlight on a pillar is bright above and below too). Looked for in
// the picture at half size; their colour is the coloured light just round the bright middle.
void find_lamps(const std::vector<uint8_t>& rgb, const std::function<bool(float, float)>& may_be_lamp) {
	constexpr int W = CREATURES1_WORLD_WIDTH, H = CREATURES1_WORLD_HEIGHT, HW = W / 2, HH = H / 2;
	std::vector<float> half(static_cast<size_t>(HW * HH * 3));
	for (int y = 0; y < HH; ++y) {
		for (int x = 0; x < HW; ++x) {
			for (int c = 0; c < 3; ++c) {
				auto p = [&](int dx, int dy) { return static_cast<float>(rgb[static_cast<size_t>(((y * 2 + dy) * W + x * 2 + dx) * 3 + c)]); };
				half[static_cast<size_t>((y * HW + x) * 3 + c)] = (p(0, 0) + p(1, 0) + p(0, 1) + p(1, 1)) / 4;
			}
		}
	}
	auto at = [&](int x, int y, int c) {
		x = ((x % HW) + HW) % HW;
		y = std::clamp(y, 0, HH - 1);
		return half[static_cast<size_t>((y * HW + x) * 3 + c)];
	};
	// the brightest channel, averaged over 3 x 3
	std::vector<float> bright(static_cast<size_t>(HW * HH));
	for (int y = 0; y < HH; ++y) {
		for (int x = 0; x < HW; ++x) {
			float best = 0;
			for (int c = 0; c < 3; ++c) {
				float sum = 0;
				for (int dy = -1; dy <= 1; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						sum += at(x + dx, y + dy, c);
					}
				}
				best = std::max(best, sum / 9);
			}
			bright[static_cast<size_t>(y * HW + x)] = best;
		}
	}
	struct Found {
		float x, y, score;
	};
	std::vector<Found> found;
	for (int y = 4; y < HH - 4; ++y) {
		for (int x = 0; x < HW; ++x) {
			const float b = bright[static_cast<size_t>(y * HW + x)];
			if (b < 150) {
				continue;
			}
			bool peak = true;
			for (int dy = -4; dy <= 4 && peak; ++dy) {
				for (int dx = -4; dx <= 4; ++dx) {
					const int xx = ((x + dx) % HW + HW) % HW;
					const float o = bright[static_cast<size_t>((y + dy) * HW + xx)];
					if (o > b || (o == b && (dy < 0 || (dy == 0 && dx < 0)))) {
						peak = false;
						break;
					}
				}
			}
			if (!peak || !may_be_lamp(static_cast<float>(x * 2), static_cast<float>(y * 2))) {
				continue;
			}
			// in each channel bright enough: the brightest of eight directions round it
			float score = 0;
			for (int c = 0; c < 3; ++c) {
				float mean = 0;
				for (int dy = -1; dy <= 1; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						mean += at(x + dx, y + dy, c);
					}
				}
				mean /= 9;
				if (mean < 150) {
					continue;
				}
				float worst = 0;
				for (int k = 0; k < 8; ++k) {
					float sum = 0;
					for (int a = 0; a < 3; ++a) {
						const float angle = (static_cast<float>(k) + (static_cast<float>(a) + 0.5f) / 3) * PI / 4;
						for (float r : {7.f, 9.f, 11.f}) {
							sum += at(x + static_cast<int>(std::lround(std::cos(angle) * r)), y + static_cast<int>(std::lround(std::sin(angle) * r)), c);
						}
					}
					worst = std::max(worst, sum / 9);
				}
				score = std::max(score, 1 - worst / mean);
			}
			if (score >= 0.62f) {
				found.push_back({static_cast<float>(x * 2 + 1), static_cast<float>(y * 2 + 1), score});
			}
		}
	}
	std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.score > b.score; });
	for (const auto& f : found) {
		bool near = false;
		for (const auto& l : s_world.lamps) {
			near = near || (l.x - f.x) * (l.x - f.x) + (l.y - f.y) * (l.y - f.y) < 16 * 16;
		}
		if (near || s_world.lamps.size() >= 200) {
			continue;
		}
		// the colour: the bright, coloured pixels round the middle count most
		const int cx = static_cast<int>(f.x), cy = static_cast<int>(f.y);
		float peak = 0;
		for (int y = cy - 2; y <= cy + 2; ++y) {
			for (int x = cx - 2; x <= cx + 2; ++x) {
				if (y >= 0 && y < H) {
					const auto* p = &rgb[static_cast<size_t>((y * W + ((x % W) + W) % W) * 3)];
					peak = std::max(peak, 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]);
				}
			}
		}
		float sum[3] = {0, 0, 0}, weight = 0;
		for (int y = cy - 7; y <= cy + 7; ++y) {
			for (int x = cx - 7; x <= cx + 7; ++x) {
				if (y < 0 || y >= H) {
					continue;
				}
				const auto* p = &rgb[static_cast<size_t>((y * W + ((x % W) + W) % W) * 3)];
				const float l = 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2];
				const float hi = std::max({p[0], p[1], p[2]}), lo = std::min({p[0], p[1], p[2]});
				const float sat = hi > 0 ? (hi - lo) / hi : 0;
				if (l < 0.45f * peak) {
					continue;
				}
				const float w = (sat * sat + 0.02f) * (l / 255) * (l / 255);
				for (int c = 0; c < 3; ++c) {
					sum[c] += p[c] * w;
				}
				weight += w;
			}
		}
		float r = 1, g = 0.8f, b = 0.5f;
		if (weight > 0) {
			const float most = std::max({sum[0], sum[1], sum[2]});
			r = sum[0] / most, g = sum[1] / most, b = sum[2] / most;
			// a little richer, so its light reads as its colour
			r = std::pow(r, 1.4f), g = std::pow(g, 1.4f), b = std::pow(b, 1.4f);
		}
		s_world.lamps.push_back({f.x, f.y, hash(static_cast<uint32_t>(f.x), static_cast<uint32_t>(f.y)) * 2 * PI, r, g, b});
	}
}

// Works out what every cell is, from the rooms and the ground, then looks at the background
// picture for the sea (blue below sea level), the sky (smooth blue, for the stars) and the
// lamps; and places the fireflies and the sea's shafts of light.
void look_at_world() {
	s_world = WorldLook{};
	s_world.generation = g_engine_context.generation();
	auto& map = *g_engine_context.map;
	const auto& ground = map.get_groundlevel();
	auto ground_at = [&](int x) {
		x = ((x % CREATURES1_WORLD_WIDTH) + CREATURES1_WORLD_WIDTH) % CREATURES1_WORLD_WIDTH;
		const int strip = std::clamp(x / 32, 0, static_cast<int>(ground.size()) - 1);
		const int y = static_cast<int>(ground[static_cast<size_t>(strip)]);
		return y > 0 ? y : CREATURES1_WORLD_HEIGHT;
	};
	// type 0 is a room indoors or underground (batch 4): a building's, or a cellar's just
	// below the ground (such as the kitchen), is lit as indoors; deeper ones are underground
	std::vector<bool> in_room(static_cast<size_t>(COLS * ROWS));
	auto indoors = [&](const Room& room) { return room.top < ground_at((room.left + room.right) / 2) + 220; };
	for (int row = 0; row < ROWS; ++row) {
		for (int col = 0; col < COLS; ++col) {
			const float x = static_cast<float>(col * CELL + CELL / 2), y = static_cast<float>(row * CELL + CELL / 2);
			Kind k;
			const Room* room = map.find_room_containing(x, y);
			in_room[static_cast<size_t>(row * COLS + col)] = room != nullptr;
			if (room) {
				k = room->type == 0 ? (indoors(*room) ? INDOOR : UNDERGROUND) : OPEN;
			} else {
				k = y < static_cast<float>(ground_at(static_cast<int>(x))) ? OPEN : SOLID;
			}
			s_world.kind[static_cast<size_t>(row * COLS + col)] = k;
		}
	}
	// open air with a roof not far above it (the kitchen's room is an outdoor one) is lit as
	// indoors too; the ground under a high treehouse stays outdoors
	for (int col = 0; col < COLS; ++col) {
		int roof = -100;
		for (int row = 0; row < ROWS; ++row) {
			auto& k = s_world.kind[static_cast<size_t>(row * COLS + col)];
			if (k == OPEN && row - roof <= 14) {
				k = INDOOR;
			} else if (k == SOLID || k == INDOOR || k == UNDERGROUND) {
				roof = row;
			}
		}
	}
	for (const auto& room : map.get_rooms()) {
		const int col = (room.left + room.right) / 2 / CELL, row = std::clamp((room.top + room.bottom) / 2 / CELL, 0, ROWS - 1);
		if (kind_at(col, row) == INDOOR) {
			s_world.rooms.push_back({static_cast<float>(room.left), static_cast<float>(room.top), static_cast<float>(room.width()), static_cast<float>(room.bottom - room.top)});
		}
	}

	// the background, 58 columns of 8 tiles 144 x 150, in the game's palette
	const auto path = g_engine_context.paths->find_path(PATH_TYPE_IMAGE, "back.spr");
	const auto& palette = g_engine_context.images->default_palette();
	if (path.empty() || !palette) {
		return;
	}
	MultiImage tiles;
	try {
		FileReader in(path);
		tiles = ReadSprFile(in);
	} catch (const std::exception& e) {
		fmt::print("WARN [DayNight] couldn't read the background: {}\n", e.what());
		return;
	}
	constexpr int W = CREATURES1_WORLD_WIDTH, H = CREATURES1_WORLD_HEIGHT;
	std::vector<uint8_t> rgb(static_cast<size_t>(W * H * 3));
	for (size_t i = 0; i < tiles.size() && i < 58 * 8; ++i) {
		const Image& tile = tiles[i];
		if (tile.format != if_index8 || !tile.data) {
			continue;
		}
		const int left = static_cast<int>(i / 8) * 144, top = static_cast<int>(i % 8) * 150;
		for (int y = 0; y < tile.height && top + y < H; ++y) {
			for (int x = 0; x < tile.width && left + x < W; ++x) {
				const Color c = palette[tile.data[static_cast<size_t>(y * tile.width + x)]];
				uint8_t* p = &rgb[static_cast<size_t>(((top + y) * W + left + x) * 3)];
				p[0] = c.r, p[1] = c.g, p[2] = c.b;
			}
		}
	}
	auto pixel = [&](int x, int y) { return &rgb[static_cast<size_t>((y * W + x) * 3)]; };

	// the sea
	for (int row = SEA_LEVEL / CELL; row < ROWS; ++row) {
		for (int col = 0; col < COLS; ++col) {
			int blue = 0, seen = 0;
			for (int y = row * CELL; y < row * CELL + CELL; y += 2) {
				for (int x = col * CELL; x < col * CELL + CELL; x += 2) {
					const uint8_t* c = pixel(x, y);
					seen++;
					blue += c[2] > c[0] + 25 && c[2] + 20 >= c[1] && c[2] > 70;
				}
			}
			const auto cell = static_cast<size_t>(row * COLS + col);
			// (the ground level runs along the sea's surface, so the sea below it counts, and
			// the blue sky above it outside the rooms doesn't)
			const Kind k = s_world.kind[cell];
			const bool below_surface = (row + 1) * CELL >= ground_at(col * CELL + CELL / 2);
			if (((k == OPEN && (in_room[cell] || below_surface)) || k == SOLID) && blue * 2 > seen) {
				s_world.kind[cell] = WATER;
			}
		}
	}
	// how the water ripples with light: open to the sky above, and less the deeper it is
	for (int col = 0; col < COLS; ++col) {
		bool open = true;
		int surface = 0;
		for (int row = 0; row < ROWS; ++row) {
			const Kind k = kind_at(col, row);
			const auto cell = static_cast<size_t>(row * COLS + col);
			if (k == WATER) {
				if (row == 0 || kind_at(col, row - 1) != WATER) {
					surface = row;
				}
				const float depth = static_cast<float>((row - surface) * CELL);
				s_world.wet[cell] = open ? std::exp(-depth / 300) : 0;
				s_world.sea[cell] = open ? 1 : 0;
			} else if (k != OPEN) {
				open = false;
			}
		}
	}
	// the shafts of light, every 70 pixels or so where the sea is open and deep
	for (int x = 0; x < W; x += 70) {
		const float rx = static_cast<float>(x) + hash(static_cast<uint32_t>(x), 7) * 40;
		const int col = static_cast<int>(rx) / CELL;
		for (int row = 1; row < ROWS; ++row) {
			if (kind_at(col, row) == WATER && kind_at(col, row - 1) != WATER && s_world.sea[static_cast<size_t>(row * COLS + col)] > 0) {
				int bottom = row;
				while (bottom < ROWS && kind_at(col, bottom) == WATER) {
					bottom++;
				}
				if ((bottom - row) * CELL >= 128) {
					s_world.rays.push_back({rx, static_cast<float>(row * CELL), static_cast<float>((bottom - row) * CELL), hash(static_cast<uint32_t>(x), 11)});
				}
				break;
			}
		}
	}

	// the sky: smooth blue in open air, kept away from the edges of hills and clouds
	{
		std::vector<float> blue(static_cast<size_t>(SKY_COLS * SKY_ROWS)), lum(blue.size());
		for (int r = 0; r < SKY_ROWS; ++r) {
			for (int c = 0; c < SKY_COLS; ++c) {
				float b = 0, l = 0;
				for (int y = 0; y < SKY_CELL; ++y) {
					for (int x = 0; x < SKY_CELL; ++x) {
						const uint8_t* p = pixel(c * SKY_CELL + x, r * SKY_CELL + y);
						b += p[2] > p[0] + 22 && p[2] > 120 && p[2] >= p[1] + 5;
						l += 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2];
					}
				}
				blue[static_cast<size_t>(r * SKY_COLS + c)] = b / (SKY_CELL * SKY_CELL);
				lum[static_cast<size_t>(r * SKY_COLS + c)] = l / (SKY_CELL * SKY_CELL);
			}
		}
		auto around = [&](const std::vector<float>& v, int c, int r, int reach, bool squared) {
			float sum = 0, n = 0;
			for (int dy = -reach; dy <= reach; ++dy) {
				for (int dx = -reach; dx <= reach; ++dx) {
					const int rr = r + dy, cc = ((c + dx) % SKY_COLS + SKY_COLS) % SKY_COLS;
					if (rr < 0 || rr >= SKY_ROWS) {
						continue;
					}
					const float x = v[static_cast<size_t>(rr * SKY_COLS + cc)];
					sum += squared ? x * x : x;
					n++;
				}
			}
			return sum / n;
		};
		std::vector<float> sky(blue.size());
		for (int r = 0; r < SKY_ROWS; ++r) {
			for (int c = 0; c < SKY_COLS; ++c) {
				const float m = around(lum, c, r, 2, false), v = around(lum, c, r, 2, true) - m * m;
				const Kind k = kind_at(c * SKY_CELL / CELL, r * SKY_CELL / CELL);
				sky[static_cast<size_t>(r * SKY_COLS + c)] = blue[static_cast<size_t>(r * SKY_COLS + c)] > 0.6f && v < 120 && k == OPEN;
			}
		}
		for (int r = 0; r < SKY_ROWS; ++r) {
			for (int c = 0; c < SKY_COLS; ++c) {
				const auto i = static_cast<size_t>(r * SKY_COLS + c);
				s_world.sky[i] = sky[i] > 0 && around(sky, c, r, 3, false) > 0.8f;
			}
		}
		// the stars: about one in 45 of the sky's cells, most of them faint
		for (int r = 0; r < SKY_ROWS; ++r) {
			for (int c = 0; c < SKY_COLS; ++c) {
				const auto i = static_cast<size_t>(r * SKY_COLS + c);
				if (!s_world.sky[i] || hash(static_cast<uint32_t>(i), 1) > 1.f / 45) {
					continue;
				}
				const float h = hash(static_cast<uint32_t>(i), 2), tint = hash(static_cast<uint32_t>(i), 3);
				Star s;
				s.x = static_cast<float>(c * SKY_CELL) + hash(static_cast<uint32_t>(i), 4) * SKY_CELL;
				s.y = static_cast<float>(r * SKY_CELL) + hash(static_cast<uint32_t>(i), 5) * SKY_CELL;
				s.bright = 0.22f + 0.78f * h * h * h;
				s.size = 1.0f + 1.6f * h * h;
				s.phase = hash(static_cast<uint32_t>(i), 6) * 2 * PI;
				s.speed = 1.2f + 3.0f * hash(static_cast<uint32_t>(i), 8);
				// a few bluish, a few yellowish
				s.r = tint < 0.2f ? 0.75f : 1.f;
				s.g = tint < 0.2f ? 0.85f : (tint > 0.85f ? 0.92f : 1.f);
				s.b = tint > 0.85f ? 0.7f : 1.f;
				s_world.stars.push_back(s);
			}
		}
	}

	// the fireflies: in open air over the land, a little above the ground
	for (int x = 0; x < W; x += 40) {
		if (hash(static_cast<uint32_t>(x), 21) > 0.55f) {
			continue;
		}
		const float fx = static_cast<float>(x) + hash(static_cast<uint32_t>(x), 22) * 40;
		const int g = ground_at(static_cast<int>(fx));
		const float fy = static_cast<float>(g) - 22 - hash(static_cast<uint32_t>(x), 23) * 110;
		const int col = static_cast<int>(fx) / CELL, row = static_cast<int>(fy) / CELL;
		if (g >= CREATURES1_WORLD_HEIGHT || row < 1 || kind_at(col, row) != OPEN) {
			continue;
		}
		bool clear = true;
		for (int dy = -1; dy <= 2 && clear; ++dy) {
			for (int dx = -1; dx <= 1; ++dx) {
				const Kind k = kind_at(col + dx, row + dy);
				clear = clear && k != INDOOR && k != WATER && (dy == 2 || k == OPEN);
			}
		}
		if (!clear || kind_at(col, (g - 8) / CELL) == WATER) {
			continue;
		}
		auto h = [&](uint32_t n) { return hash(static_cast<uint32_t>(x), n); };
		s_world.fireflies.push_back({fx, fy, h(24) * 2 * PI, h(25) * 2 * PI, h(26) * 2 * PI, h(27) * 2 * PI, 2.6f + 3.5f * h(28), h(29)});
	}

	// the lamps: underground, indoors, under water, or in the ground
	find_lamps(rgb, [&](float x, float y) {
		const Kind k = kind_at(static_cast<int>(x) / CELL, static_cast<int>(y) / CELL);
		return k == UNDERGROUND || k == INDOOR || k == WATER || (k == SOLID && y >= static_cast<float>(ground_at(static_cast<int>(x)) - 40));
	});
	fmt::print("* Day and night: {} lamps, {} stars, {} fireflies, {} shafts of light\n", s_world.lamps.size(), s_world.stars.size(), s_world.fireflies.size(), s_world.rays.size());

	// debugging: OPENCREATURES1_TEST_LIGHT_DUMP=<file.ppm> saves what each cell is, with
	// the lamps in their colours
	if (const char* dump = std::getenv("OPENCREATURES1_TEST_LIGHT_DUMP")) {
		std::vector<uint8_t> out(static_cast<size_t>(COLS * ROWS * 3));
		static const uint8_t colours[5][3] = {{150, 190, 240}, {230, 150, 60}, {120, 70, 40}, {50, 40, 30}, {30, 80, 200}};
		for (size_t i = 0; i < s_world.kind.size(); ++i) {
			std::copy(colours[s_world.kind[i]], colours[s_world.kind[i]] + 3, &out[i * 3]);
		}
		for (const auto& l : s_world.lamps) {
			const auto i = static_cast<size_t>(static_cast<int>(l.y) / CELL * COLS + static_cast<int>(l.x) / CELL);
			out[i * 3] = static_cast<uint8_t>(l.r * 255), out[i * 3 + 1] = static_cast<uint8_t>(l.g * 255), out[i * 3 + 2] = static_cast<uint8_t>(l.b * 255);
		}
		if (FILE* f = std::fopen(dump, "wb")) {
			std::fprintf(f, "P6 %d %d 255\n", COLS, ROWS);
			std::fwrite(out.data(), 1, out.size(), f);
			std::fclose(f);
		}
	}
}

// The light for every cell at a time of day, as colours to multiply the world by.
void work_out_light(float t) {
	const Sun sun = sun_at(t);
	const float day = sun.strength;
	// the step towards the sun, a cell at a time along its longer axis
	const float along = std::max(std::abs(sun.dx), std::abs(sun.dy));
	const float stepx = along > 0 ? sun.dx / along : 0, stepy = along > 0 ? sun.dy / along : 0;
	std::vector<float> r(static_cast<size_t>(COLS * ROWS)), g(r.size()), b(r.size());
	for (int row = 0; row < ROWS; ++row) {
		for (int col = 0; col < COLS; ++col) {
			const auto cell = static_cast<size_t>(row * COLS + col);
			const Kind k = s_world.kind[cell];
			// what reaches it from the sun: nothing through the ground, walls or roofs, less
			// through water
			float through = 0;
			if (sun.up && (k == OPEN || k == WATER)) {
				through = 1;
				float x = static_cast<float>(col) + 0.5f, y = static_cast<float>(row) + 0.5f;
				for (int i = 0; i < 64 && through > 0.02f; ++i) {
					x += stepx;
					y += stepy;
					if (y < 0) {
						break;
					}
					const Kind on = kind_at(static_cast<int>(std::floor(x)), std::min(static_cast<int>(y), ROWS - 1));
					if (on == SOLID || on == INDOOR || on == UNDERGROUND) {
						through = 0;
					} else if (on == WATER) {
						through *= 0.86f;
					}
				}
				if (k == WATER) {
					through *= 0.86f;
				}
			}
			const float direct = day * through * 0.42f;
			float lr, lg, lb;
			switch (k) {
				case OPEN:
				case WATER:
					// the sky's light, blue at night and golden when the sun is low
					lr = lerp(0.26f, lerp(0.60f, 0.66f, sun.warm), day) + direct * sun.r;
					lg = lerp(0.30f, lerp(0.60f, 0.50f, sun.warm), day) + direct * sun.g;
					lb = lerp(0.48f, lerp(0.62f, 0.44f, sun.warm), day) + direct * sun.b;
					if (k == WATER) {
						// blue-green under the sea
						lr *= 0.62f;
						lg *= 0.88f;
					}
					break;
				case INDOOR:
					// lamplight indoors: warm, and cosier at night
					lr = lerp(0.70f, 0.97f, day);
					lg = lerp(0.55f, 0.90f, day);
					lb = lerp(0.40f, 0.80f, day);
					break;
				case UNDERGROUND:
					lr = 0.66f, lg = 0.58f, lb = 0.52f;
					break;
				default:
					lr = lerp(0.48f, 0.66f, day), lg = lerp(0.46f, 0.62f, day), lb = lerp(0.46f, 0.58f, day);
					break;
			}
			r[cell] = std::min(lr, 1.f);
			g[cell] = std::min(lg, 1.f);
			b[cell] = std::min(lb, 1.f);
		}
	}
	// soft edges: two passes of a box blur
	for (int pass = 0; pass < 2; ++pass) {
		for (auto* channel : {&r, &g, &b}) {
			std::vector<float> out(channel->size());
			for (int row = 0; row < ROWS; ++row) {
				for (int col = 0; col < COLS; ++col) {
					float sum = 0;
					int n = 0;
					for (int dy = -1; dy <= 1; ++dy) {
						const int y = row + dy;
						if (y < 0 || y >= ROWS) {
							continue;
						}
						for (int dx = -1; dx <= 1; ++dx) {
							const int x = ((col + dx) % COLS + COLS) % COLS;
							sum += (*channel)[static_cast<size_t>(y * COLS + x)];
							n++;
						}
					}
					out[static_cast<size_t>(row * COLS + col)] = sum / static_cast<float>(n);
				}
			}
			*channel = std::move(out);
		}
	}
	for (size_t i = 0; i < r.size(); ++i) {
		s_light.rgba[i * 4 + 0] = static_cast<uint8_t>(r[i] * 255);
		s_light.rgba[i * 4 + 1] = static_cast<uint8_t>(g[i] * 255);
		s_light.rgba[i * 4 + 2] = static_cast<uint8_t>(b[i] * 255);
		s_light.rgba[i * 4 + 3] = 255;
	}
	s_light.made_for = t;
}

SDL_Texture* make_texture(SDL_Renderer* renderer, int w, int h, const std::vector<uint8_t>& pixels, SDL_BlendMode blend) {
	SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
	if (texture) {
		SDL_UpdateTexture(texture, nullptr, pixels.data(), w * 4);
		SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
		SDL_SetTextureBlendMode(texture, blend);
	}
	return texture;
}

// light rippling under water: the edges of a tiling pattern of cells, which is how light
// gathers under waves
std::vector<uint8_t> ripple_pixels(int size) {
	constexpr int GRID = 9;
	const float cell = static_cast<float>(size) / GRID;
	std::vector<float> px(static_cast<size_t>(GRID * GRID)), py(px.size());
	for (int i = 0; i < GRID * GRID; ++i) {
		px[static_cast<size_t>(i)] = (static_cast<float>(i % GRID) + 0.15f + 0.7f * hash(static_cast<uint32_t>(i), 31)) * cell;
		py[static_cast<size_t>(i)] = (static_cast<float>(i / GRID) + 0.15f + 0.7f * hash(static_cast<uint32_t>(i), 32)) * cell;
	}
	std::vector<float> v(static_cast<size_t>(size * size));
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			const int gx = static_cast<int>(static_cast<float>(x) / cell), gy = static_cast<int>(static_cast<float>(y) / cell);
			float f1 = 1e9f, f2 = 1e9f;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					const int cx = gx + dx, cy = gy + dy;
					const int i = ((cy + GRID) % GRID) * GRID + (cx + GRID) % GRID;
					const float ox = px[static_cast<size_t>(i)] + static_cast<float>(cx - (cx + GRID) % GRID) * cell;
					const float oy = py[static_cast<size_t>(i)] + static_cast<float>(cy - (cy + GRID) % GRID) * cell;
					const float d = std::hypot(static_cast<float>(x) - ox, static_cast<float>(y) - oy);
					if (d < f1) {
						f2 = f1;
						f1 = d;
					} else if (d < f2) {
						f2 = d;
					}
				}
			}
			const float edge = 1 - smoothstep(0, cell * 0.16f, f2 - f1);
			v[static_cast<size_t>(y * size + x)] = edge * edge * 0.85f + 0.15f * (1 - std::min(1.f, f1 / cell));
		}
	}
	std::vector<uint8_t> pixels(static_cast<size_t>(size * size * 4));
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			float sum = 0;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					sum += v[static_cast<size_t>(((y + dy + size) % size) * size + (x + dx + size) % size)];
				}
			}
			uint8_t* p = &pixels[static_cast<size_t>((y * size + x) * 4)];
			p[0] = p[1] = p[2] = static_cast<uint8_t>(std::clamp(sum / 9, 0.f, 1.f) * 255);
			p[3] = 255;
		}
	}
	return pixels;
}

void make_textures(SDL_Renderer* renderer) {
	if (s_light.renderer == renderer && s_light.texture) {
		return;
	}
	s_light.renderer = renderer;
	s_light.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, COLS, ROWS);
	if (s_light.texture) {
		SDL_SetTextureScaleMode(s_light.texture, SDL_SCALEMODE_LINEAR);
	}
	constexpr int SIZE = 128;
	std::vector<uint8_t> pixels(SIZE * SIZE * 4);
	for (int y = 0; y < SIZE; ++y) {
		for (int x = 0; x < SIZE; ++x) {
			const float dx = (static_cast<float>(x) + 0.5f) / SIZE * 2 - 1, dy = (static_cast<float>(y) + 0.5f) / SIZE * 2 - 1;
			const float d = std::min(1.f, std::sqrt(dx * dx + dy * dy));
			uint8_t* p = &pixels[static_cast<size_t>((y * SIZE + x) * 4)];
			p[0] = p[1] = p[2] = 255;
			p[3] = static_cast<uint8_t>((1 - d) * (1 - d) * 255);
		}
	}
	s_light.glow = make_texture(renderer, SIZE, SIZE, pixels, SDL_BLENDMODE_ADD);
	constexpr int SPARK = 32;
	pixels.assign(SPARK * SPARK * 4, 255);
	for (int y = 0; y < SPARK; ++y) {
		for (int x = 0; x < SPARK; ++x) {
			const float dx = std::abs((static_cast<float>(x) + 0.5f) / SPARK * 2 - 1), dy = std::abs((static_cast<float>(y) + 0.5f) / SPARK * 2 - 1);
			const float core = std::exp(-(dx * dx + dy * dy) * 30);
			const float arms = std::exp(-dx * 40) * (1 - dy) + std::exp(-dy * 40) * (1 - dx);
			pixels[static_cast<size_t>((y * SPARK + x) * 4 + 3)] = static_cast<uint8_t>(std::clamp(core + arms * 0.8f, 0.f, 1.f) * 255);
		}
	}
	s_light.sparkle = make_texture(renderer, SPARK, SPARK, pixels, SDL_BLENDMODE_ADD);
	s_light.ripples = make_texture(renderer, 256, 256, ripple_pixels(256), SDL_BLENDMODE_ADD);
	// the world lit by the light map: twice the world times the map, so a map of half
	// leaves it as it is and lights can brighten it
	s_light.lit = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_DST_COLOR, SDL_BLENDFACTOR_SRC_COLOR, SDL_BLENDOPERATION_ADD,
		SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
	s_light.can_light = false;
	if (SDL_Texture* test = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 4, 4)) {
		s_light.can_light = SDL_SetTextureBlendMode(test, s_light.lit);
		SDL_DestroyTexture(test);
	}
	if (!s_light.can_light) {
		fmt::print("* Day and night: the renderer can't light the world with a light map; shading it instead\n");
	}
	s_light.made_for = -1;
}

// Where a point of the world is drawn on something covering the view (the window, or the
// light map), allowing for the world wrapping round.
struct Place {
	float x0, y0, kx, ky; // the view's corner there, and its scale from the world
	float cam_x, cam_y, cam_w, cam_h; // the camera, its left edge within the world

	bool at(float wx, float wy, float margin, float& x, float& y) const {
		float dx = wx - cam_x;
		dx -= std::floor(dx / CREATURES1_WORLD_WIDTH) * CREATURES1_WORLD_WIDTH;
		if (dx > CREATURES1_WORLD_WIDTH - margin) {
			dx -= CREATURES1_WORLD_WIDTH;
		}
		const float dy = wy - cam_y;
		if (dx < -margin || dx > cam_w + margin || dy < -margin || dy > cam_h + margin) {
			return false;
		}
		x = x0 + dx * kx;
		y = y0 + dy * ky;
		return true;
	}
};

void glow_at(SDL_Renderer* renderer, SDL_Texture* texture, float x, float y, float rx, float ry, float r, float g, float b, float strength) {
	if (strength <= 0.003f) {
		return;
	}
	SDL_SetTextureColorModFloat(texture, r, g, b);
	SDL_SetTextureAlphaModFloat(texture, std::clamp(strength, 0.f, 1.f));
	const SDL_FRect to{x - rx, y - ry, rx * 2, ry * 2};
	SDL_RenderTexture(renderer, texture, nullptr, &to);
}

// a glow of a radius in world pixels at a world point
void world_glow(SDL_Renderer* renderer, const Place& p, SDL_Texture* texture, float wx, float wy, float radius, float r, float g, float b, float strength) {
	float x, y;
	if (p.at(wx, wy, radius, x, y)) {
		glow_at(renderer, texture, x, y, radius * p.kx, radius * p.ky, r, g, b, strength);
	}
}

// the cells' light over the view
void draw_cells(SDL_Renderer* renderer, const Place& p) {
	const float top = std::max(0.f, p.cam_y), bottom = std::min(static_cast<float>(CREATURES1_WORLD_HEIGHT), p.cam_y + p.cam_h);
	if (bottom <= top) {
		return;
	}
	float x = p.cam_x, left = 0;
	while (left < p.cam_w) {
		const float width = std::min(p.cam_w - left, static_cast<float>(CREATURES1_WORLD_WIDTH) - x);
		const SDL_FRect from{x / CELL, top / CELL, width / CELL, (bottom - top) / CELL};
		const SDL_FRect to{p.x0 + left * p.kx, p.y0 + (top - p.cam_y) * p.ky, width * p.kx, (bottom - top) * p.ky};
		SDL_RenderTexture(renderer, s_light.texture, &from, &to);
		left += width;
		x = 0;
	}
}

// Light rippling in the water near its surface: the pattern twice, drifting different ways,
// over the cells of open water in view, fading with depth.
void draw_ripples(SDL_Renderer* renderer, const Place& p, float seconds, float strength) {
	if (strength <= 0.003f || !s_light.ripples) {
		return;
	}
	const int c0 = static_cast<int>(std::floor(p.cam_x / CELL)) - 1, c1 = c0 + static_cast<int>(p.cam_w / CELL) + 3;
	const int r0 = std::max(0, static_cast<int>(std::floor(p.cam_y / CELL)) - 1), r1 = std::min(ROWS, static_cast<int>((p.cam_y + p.cam_h) / CELL) + 2);
	if (r1 <= r0) {
		return;
	}
	const int vw = c1 - c0 + 1, vh = r1 - r0 + 1;
	auto wet = [&](int c, int r) {
		c = ((c % COLS) + COLS) % COLS;
		return r < 0 || r >= ROWS ? 0.f : s_world.wet[static_cast<size_t>(r * COLS + c)];
	};
	std::vector<float> corner(static_cast<size_t>(vw * vh));
	bool any = false;
	for (int j = 0; j < vh; ++j) {
		for (int i = 0; i < vw; ++i) {
			const int c = c0 + i, r = r0 + j;
			const float v = (wet(c - 1, r - 1) + wet(c, r - 1) + wet(c - 1, r) + wet(c, r)) / 4;
			corner[static_cast<size_t>(j * vw + i)] = v;
			any = any || v > 0;
		}
	}
	if (!any) {
		return;
	}
	std::vector<int> indices;
	for (int j = 0; j + 1 < vh; ++j) {
		for (int i = 0; i + 1 < vw; ++i) {
			const int a = j * vw + i;
			if (corner[static_cast<size_t>(a)] + corner[static_cast<size_t>(a + 1)] + corner[static_cast<size_t>(a + vw)] + corner[static_cast<size_t>(a + vw + 1)] <= 0) {
				continue;
			}
			indices.insert(indices.end(), {a, a + 1, a + vw, a + 1, a + vw + 1, a + vw});
		}
	}
	const float left = static_cast<float>(c0 * CELL) - p.cam_x;
	// tiles of 288 and 232 pixels, which both go into the world's width, so nothing jumps
	// where it wraps round
	struct Layer {
		float tile, vx, vy, weight;
	};
	std::vector<SDL_Vertex> vertices(static_cast<size_t>(vw * vh));
	for (const Layer layer : {Layer{288, 9, 5, 1.f}, Layer{232, -7, 6, 0.8f}}) {
		for (int j = 0; j < vh; ++j) {
			for (int i = 0; i < vw; ++i) {
				const float wx = static_cast<float>((c0 + i) * CELL), wy = static_cast<float>((r0 + j) * CELL);
				auto& v = vertices[static_cast<size_t>(j * vw + i)];
				v.position = {p.x0 + (left + static_cast<float>(i * CELL)) * p.kx, p.y0 + (wy - p.cam_y) * p.ky};
				const float k = corner[static_cast<size_t>(j * vw + i)] * strength * layer.weight;
				v.color = {0.62f * k, 0.9f * k, 1.f * k, 1.f};
				v.tex_coord = {(wx + seconds * layer.vx) / layer.tile, (wy + seconds * layer.vy + 6 * std::sin(seconds * 0.4f + wx * 0.01f)) / layer.tile};
			}
		}
		SDL_RenderGeometry(renderer, s_light.ripples, vertices.data(), static_cast<int>(vertices.size()), indices.data(), static_cast<int>(indices.size()));
	}
}

// The shafts of light down through the sea, slanting away from the sun (less steeply in the
// water), swaying, and each fading in and out in its own time.
void draw_rays(SDL_Renderer* renderer, const Place& p, float seconds, const Sun& light, float strength, float r, float g, float b) {
	if (strength <= 0.003f || s_world.rays.empty()) {
		return;
	}
	const float in_air = std::atan2(std::abs(light.dx), std::max(0.05f, std::abs(light.dy)));
	const float in_water = std::asin(std::sin(in_air) / 1.33f);
	const float side = light.dx > 0 ? -1.f : 1.f;
	std::vector<SDL_Vertex> vertices;
	std::vector<int> indices;
	constexpr int STEPS = 8;
	for (const auto& ray : s_world.rays) {
		float x, y;
		if (!p.at(ray.x, ray.top, 500, x, y)) {
			continue;
		}
		const float angle = in_water + 0.05f * std::sin(seconds * 0.3f + ray.seed * 40);
		const float dx = std::sin(angle) * side, dy = std::cos(angle);
		const float fade = std::pow(0.5f + 0.5f * std::sin(seconds * 0.23f + ray.seed * 70), 1.5f) * (0.7f + 0.3f * std::sin(seconds * 0.71f + ray.seed * 30));
		const float a = strength * fade;
		if (a <= 0.003f) {
			continue;
		}
		const float length = std::min(ray.depth * 0.95f, 220 + 260 * ray.seed) / dy;
		const float w0 = 14 + 34 * hash(static_cast<uint32_t>(ray.x), 41), w1 = w0 * 1.9f;
		const float top_x = ray.x + 8 * std::sin(seconds * 0.17f + ray.seed * 20);
		const int first = static_cast<int>(vertices.size());
		for (int s = 0; s <= STEPS; ++s) {
			const float u = static_cast<float>(s) / STEPS;
			const float cx = top_x + dx * length * u, cy = ray.top + dy * length * u;
			const float half = lerp(w0, w1, u) / 2;
			const float along = (1 - u) * smoothstep(0, 0.08f, u + 0.02f);
			for (int k = -1; k <= 1; ++k) {
				const float wx = cx + dy * half * static_cast<float>(k), wy = cy - dx * half * static_cast<float>(k);
				const float v = k == 0 ? a * along * std::clamp(sample(s_world.sea, wx, wy), 0.f, 1.f) : 0;
				// placed from the shaft's top, so it stays in one piece across the world's edge
				vertices.push_back({{x + (wx - ray.x) * p.kx, y + (wy - ray.top) * p.ky}, {r * v, g * v, b * v, 1.f}, {0, 0}});
			}
			if (s > 0) {
				const int row = first + (s - 1) * 3, next = row + 3;
				indices.insert(indices.end(), {row, row + 1, next, row + 1, next + 1, next, row + 1, row + 2, next + 1, row + 2, next + 2, next + 1});
			}
		}
	}
	if (!indices.empty()) {
		SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
		SDL_RenderGeometry(renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()), indices.data(), static_cast<int>(indices.size()));
	}
}

float firefly_blink(const Firefly& f, float seconds) {
	const float u = std::fmod(seconds / f.period + f.offset, 1.f);
	// a quick flash, then a long dark with a faint ember
	return u < 0.16f ? std::pow(std::sin(PI * u / 0.16f), 2.f) : 0.04f;
}

void firefly_at(const Firefly& f, float seconds, float& x, float& y) {
	x = f.x + 22 * std::sin(seconds * 0.37f + f.p1) + 9 * std::sin(seconds * 1.1f + f.p2);
	y = f.y + 14 * std::sin(seconds * 0.53f + f.p3) + 6 * std::sin(seconds * 1.7f + f.p4);
}

bool is_egg(const Object& o) {
	// 2 5 6 is the hidden Grendel mother, not an egg
	return o.family == 2 && o.genus == 5 && o.species != 6;
}

bool in_world(const Rect2f& box) {
	return box.y >= 0 && box.y < CREATURES1_WORLD_HEIGHT && box.width > 0;
}

// what lights its surroundings: the indoor rooms, the lamps, the eggs, the hand, fireflies
void draw_lights(SDL_Renderer* renderer, const Place& p, float seconds, const Sun& sun, float fire, float scale, const Glowing& shown) {
	auto* glow = s_light.glow;
	const float day = sun.strength;
	for (const auto& room : s_world.rooms) {
		world_glow(renderer, p, glow, room.x + room.w / 2, room.y + room.h * 0.45f, std::max(room.w, room.h) * 0.7f, 1.f, 0.70f, 0.42f, scale * lerp(0.22f, 0.09f, day));
	}
	for (const auto& lamp : s_world.lamps) {
		const float pulse = 0.5f + 0.5f * std::sin(seconds * 1.7f + lamp.phase);
		world_glow(renderer, p, glow, lamp.x, lamp.y, 130, lamp.r, lamp.g, lamp.b, scale * (0.20f + 0.10f * pulse));
	}
	for (const auto& box : shown.eggs) {
		world_glow(renderer, p, glow, box.x + box.width / 2, box.y + box.height / 2, 80, 0.30f, 0.55f, 1.f, scale * 0.15f);
	}
	for (const auto& box : shown.hands) {
		world_glow(renderer, p, glow, box.x + box.width / 2, box.y + box.height / 2, 70, 1.f, 0.92f, 0.75f, scale * lerp(0.13f, 0.06f, day));
	}
	if (fire > 0) {
		for (const auto& f : s_world.fireflies) {
			float x, y;
			firefly_at(f, seconds, x, y);
			world_glow(renderer, p, glow, x, y, 56, 1.f, 0.86f, 0.36f, scale * 0.16f * fire * firefly_blink(f, seconds));
		}
	}
}

// what can be seen glowing: the lamps, the eggs and their sparkles, the hand, fireflies
void draw_glowing(SDL_Renderer* renderer, const Place& p, float seconds, const Sun& sun, float fire, const Glowing& shown) {
	const float day = sun.strength;
	for (const auto& lamp : s_world.lamps) {
		const float pulse = 0.5f + 0.5f * std::sin(seconds * 1.7f + lamp.phase);
		world_glow(renderer, p, s_light.glow, lamp.x, lamp.y, 22, lamp.r, lamp.g, lamp.b, 0.28f + 0.14f * pulse);
	}
	for (size_t e = 0; e < shown.eggs.size(); ++e) {
		const auto& box = shown.eggs[e];
		const float cx = box.x + box.width / 2, cy = box.y + box.height / 2;
		const float seed = static_cast<float>(shown.seeds[e] % 97);
		const float pulse = 0.5f + 0.5f * std::sin(seconds * 2.1f + seed);
		world_glow(renderer, p, s_light.glow, cx, cy, std::max(box.width, box.height) * 0.95f + 6, 0.35f, 0.62f, 1.f, 0.16f + 0.08f * pulse);
		// tiny sparkles rising off every side of it
		for (uint32_t i = 0; i < 14; ++i) {
			const float period = 1.1f + 0.9f * hash(i, static_cast<uint32_t>(seed));
			const float t = seconds / period + hash(i, static_cast<uint32_t>(seed) + 50);
			const auto cycle = static_cast<uint32_t>(std::floor(t));
			const float u = t - std::floor(t);
			const float angle = hash(cycle, i * 7 + static_cast<uint32_t>(seed)) * 2 * PI;
			const float out = 2 + 12 * u;
			const float x = cx + std::cos(angle) * (box.width / 2 + out);
			const float y = cy + std::sin(angle) * (box.height / 2 + out) - 6 * u;
			const float a = std::pow(std::sin(PI * u), 2.f) * (0.7f + 0.3f * std::sin(seconds * 13 + static_cast<float>(i)));
			const float size = (1.6f + 1.8f * hash(cycle, i + 99)) * (1 - 0.4f * u);
			const float white = hash(cycle, i + 199);
			world_glow(renderer, p, s_light.sparkle, x, y, size * 1.8f, lerp(0.45f, 0.9f, white), lerp(0.75f, 0.95f, white), 1.f, a);
		}
	}
	for (const auto& box : shown.hands) {
		const float pulse = 0.5f + 0.5f * std::sin(seconds * 1.3f);
		world_glow(renderer, p, s_light.glow, box.x + box.width / 2, box.y + box.height / 2, std::max(box.width, box.height) * 0.9f + 8, 1.f, 0.94f, 0.78f,
			lerp(0.30f, 0.16f, day) + 0.06f * pulse);
	}
	if (fire > 0) {
		for (const auto& f : s_world.fireflies) {
			float x, y;
			firefly_at(f, seconds, x, y);
			const float b = fire * firefly_blink(f, seconds);
			world_glow(renderer, p, s_light.glow, x, y, 18, 1.f, 0.88f, 0.4f, 0.55f * b);
			world_glow(renderer, p, s_light.glow, x, y, 3.5f, 1.f, 1.f, 0.7f, std::min(1.f, 2.f * b));
		}
	}
}

// the stars, twinkling, and now and then a shooting star
// the shooting stars are only in the main view
void draw_night_sky(SDL_Renderer* renderer, const Place& p, float seconds, double now, float night, bool shooting) {
	if (night <= 0.01f) {
		if (shooting) {
			s_shooting.alive = false;
		}
		return;
	}
	const float zoom = std::sqrt(p.kx);
	for (const auto& s : s_world.stars) {
		float x, y;
		if (!p.at(s.x, s.y, 4, x, y)) {
			continue;
		}
		const float low = 1 - 0.7f * smoothstep(300, 600, s.y);
		const float twinkle = 0.72f + 0.28f * std::sin(seconds * s.speed + s.phase);
		const float a = night * s.bright * twinkle * low;
		const float radius = std::max(1.1f, s.size * zoom * 1.4f);
		glow_at(renderer, s.bright > 0.8f ? s_light.sparkle : s_light.glow, x, y, radius * (s.bright > 0.8f ? 2.2f : 1.f), radius * (s.bright > 0.8f ? 2.2f : 1.f), s.r, s.g, s.b, a);
	}

	if (!shooting) {
		return;
	}
	const auto dt = static_cast<float>(std::clamp(now - s_shooting.last, 0.0, 0.1));
	s_shooting.last = now;
	if (!s_shooting.alive && now >= s_shooting.next && night > 0.5f) {
		s_shooting.next = now + 6 + 12 * frand();
		// somewhere in the sky in view, heading down to one side
		for (int tries = 0; tries < 16; ++tries) {
			const float x = p.cam_x + frand() * p.cam_w, y = p.cam_y + frand() * p.cam_h * 0.8f;
			if (!sky_at(x, y)) {
				continue;
			}
			const float angle = (0.25f + 0.35f * frand()) * (frand() < 0.5f ? 1.f : -1.f);
			const float speed = 650 + 300 * frand();
			s_shooting = {x, y, std::cos(angle) * speed * (angle > 0 ? 1.f : -1.f), std::abs(std::sin(angle)) * speed, 0, 0.6f + 0.5f * frand(), 90 + 80 * frand(), true, s_shooting.next, now};
			break;
		}
	}
	if (s_shooting.alive) {
		s_shooting.age += dt;
		s_shooting.x += s_shooting.vx * dt;
		s_shooting.y += s_shooting.vy * dt;
		if (s_shooting.age >= s_shooting.life || !sky_at(s_shooting.x, s_shooting.y)) {
			s_shooting.alive = false;
			return;
		}
		const float a = std::sin(PI * s_shooting.age / s_shooting.life) * night;
		const float speed = std::hypot(s_shooting.vx, s_shooting.vy);
		const float ux = s_shooting.vx / speed, uy = s_shooting.vy / speed;
		float hx, hy, tx, ty;
		if (!p.at(s_shooting.x, s_shooting.y, 200, hx, hy) || !p.at(s_shooting.x - ux * s_shooting.tail, s_shooting.y - uy * s_shooting.tail, 400, tx, ty)) {
			return;
		}
		const float w = 1.2f * zoom;
		const float nx = -uy * w, ny = ux * w;
		const SDL_Vertex v[4] = {
			{{hx + nx, hy + ny}, {1.f * a, 0.97f * a, 0.9f * a, 1}, {0, 0}},
			{{hx - nx, hy - ny}, {1.f * a, 0.97f * a, 0.9f * a, 1}, {0, 0}},
			{{tx + nx * 0.3f, ty + ny * 0.3f}, {0, 0, 0, 1}, {0, 0}},
			{{tx - nx * 0.3f, ty - ny * 0.3f}, {0, 0, 0, 1}, {0, 0}},
		};
		const int indices[6] = {0, 1, 2, 1, 3, 2};
		SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
		SDL_RenderGeometry(renderer, nullptr, v, 4, indices, 6);
		glow_at(renderer, s_light.glow, hx, hy, 5 * zoom, 5 * zoom, 1.f, 0.97f, 0.9f, a);
	}
}

// the eggs and the hand in the world, or in the recording playing back
Glowing glowing() {
	Glowing g;
	if (s_playback) {
		g.eggs = s_playback->eggs;
		g.hands = s_playback->hands;
		g.seeds = s_playback->egg_seeds;
		return g;
	}
	for (const auto* obj : *g_engine_context.objects) {
		if (is_egg(*obj)) {
			const auto box = obj->get_bbox();
			if (in_world(box)) {
				g.eggs.push_back(box);
				g.seeds.push_back(obj->uid.index);
			}
		}
	}
	if (const auto* hand = g_engine_context.objects->try_get(g_engine_context.pointer->m_pointer_tool); hand && !g_engine_context.pointer->m_over_interface) {
		g.hands.push_back(hand->get_bbox());
	}
	return g;
}

} // namespace

void show_playback(const Shown* shown) {
	s_playback = shown;
}

float time_of_day(uint32_t tick) {
	// debugging: OPENCREATURES1_TEST_TIME_OF_DAY=0.5 holds it at noon
	static const char* held = std::getenv("OPENCREATURES1_TEST_TIME_OF_DAY");
	if (held) {
		return std::clamp(static_cast<float>(std::atof(held)), 0.f, 0.999f);
	}
	return static_cast<float>(tick % DAY_TICKS) / static_cast<float>(DAY_TICKS);
}

uint32_t day_number(uint32_t tick) {
	return tick / DAY_TICKS + 1;
}

void draw_dial(ImDrawList* draw, ImVec2 c, float r, float t) {
	const Sun sun = sun_at(t);
	const float day = sun.strength;
	// the sky, navy at night and blue by day, warmer when the sun is low
	const float low = day > 0 ? std::clamp(1 - std::abs(sun.dy) / 0.5f, 0.f, 1.f) : 0;
	const ImU32 sky = IM_COL32(static_cast<int>(lerp(24, lerp(112, 236, low), day)), static_cast<int>(lerp(32, lerp(170, 150, low), day)),
		static_cast<int>(lerp(70, lerp(232, 120, low), day)), 255);
	draw->AddCircleFilled(c, r, sky, 32);
	// a star or two at night
	if (day < 0.5f) {
		const ImU32 star = IM_COL32(230, 235, 255, static_cast<int>(200 * (1 - day * 2)));
		draw->AddCircleFilled(ImVec2(c.x - r * 0.45f, c.y - r * 0.5f), std::max(0.8f, r * 0.06f), star, 6);
		draw->AddCircleFilled(ImVec2(c.x + r * 0.3f, c.y - r * 0.62f), std::max(0.8f, r * 0.05f), star, 6);
	}
	// the sun by day, the moon by night, on an arc from the left horizon to the right
	const bool sun_up = day > 0;
	const float part = sun_up ? (t - 0.25f) / 0.5f : std::fmod(t + 0.25f, 1.f) / 0.5f;
	const float a = PI * (1 - std::clamp(part, 0.f, 1.f));
	const ImVec2 body(c.x + std::cos(a) * r * 0.6f, c.y - std::sin(a) * r * 0.6f + r * 0.08f);
	if (sun_up) {
		draw->AddCircleFilled(body, r * 0.36f, IM_COL32(255, 214, 90, 80), 20);
		draw->AddCircleFilled(body, r * 0.24f, IM_COL32(255, 232, 140, 255), 20);
	} else {
		draw->AddCircleFilled(body, r * 0.22f, IM_COL32(232, 236, 248, 255), 20);
		draw->AddCircleFilled(ImVec2(body.x + r * 0.1f, body.y - r * 0.05f), r * 0.19f, sky, 20);
	}
	// the ground below the horizon
	draw->PathArcTo(c, r, 0, PI, 24);
	draw->PathFillConvex(IM_COL32(66, 52, 34, 255));
	draw->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(120, 98, 60, 255), 1);
	draw->AddCircle(c, r, UIArt::BRASS, 32, 1.5f);
}

namespace {

// the light map for each view drawn (the main view, and smaller ones such as Creature's View)
struct LightMap {
	SDL_Texture* texture = nullptr;
	int w = 0, h = 0;
};
std::map<const void*, LightMap> s_maps;

bool ready(SDL_Renderer* renderer) {
	if (!renderer || GameUI::options().classic_layout || !GameUI::options().day_and_night) {
		return false;
	}
	if (s_world.generation != g_engine_context.generation()) {
		look_at_world();
		s_light.made_for = -1;
	}
	make_textures(renderer);
	return s_light.texture && s_light.glow && s_light.sparkle;
}

// The light over one view of the world: camera is the part of the world shown, view where it
// is on the current render target. The main view also has the glow at its edge and the
// shooting stars.
void light_view(SDL_Renderer* renderer, const Rect2i& camera, const Rect2f& view, const void* slot, bool main) {
	const float t = time_of_day(s_playback ? s_playback->tick : g_engine_context.world->tick);
	const Glowing shown = glowing();
	// worked out again as the sun moves, about every 4 seconds of game time
	if (s_light.made_for < 0 || std::abs(t - s_light.made_for) > 0.0011f) {
		work_out_light(t);
		SDL_UpdateTexture(s_light.texture, nullptr, s_light.rgba.data(), COLS * 4);
	}
	static const auto start = std::chrono::steady_clock::now();
	const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const auto seconds = static_cast<float>(std::fmod(now, 3600.0 * 24));
	const Sun sun = sun_at(t);
	const Sun moon = sun_at(std::fmod(t + 0.5f, 1.f));
	const float night = 1 - smoothstep(0.02f, 0.30f, sun.strength);
	const float fire = 1 - smoothstep(0.05f, 0.45f, sun.strength);

	Place screen;
	screen.cam_x = static_cast<float>(((camera.x % CREATURES1_WORLD_WIDTH) + CREATURES1_WORLD_WIDTH) % CREATURES1_WORLD_WIDTH);
	screen.cam_y = static_cast<float>(camera.y);
	screen.cam_w = static_cast<float>(camera.width);
	screen.cam_h = static_cast<float>(camera.height);
	screen.x0 = view.x, screen.y0 = view.y;
	screen.kx = view.width / screen.cam_w, screen.ky = view.height / screen.cam_h;
	// light from the sun or the moon, for the water
	const Sun& above = sun.strength > 0.02f ? sun : moon;
	const float water_light = sun.strength > 0.02f ? sun.strength : 0.2f * moon.strength;

	SDL_BlendMode draw_blend;
	SDL_GetRenderDrawBlendMode(renderer, &draw_blend);
	if (s_light.can_light) {
		// the light map for the view, at half the window's resolution
		float scale_x = 1, scale_y = 1;
		SDL_GetRenderScale(renderer, &scale_x, &scale_y);
		const int w = std::max(1, static_cast<int>(view.width * scale_x / 2)), h = std::max(1, static_cast<int>(view.height * scale_y / 2));
		LightMap& map = s_maps[slot];
		if (!map.texture || map.w != w || map.h != h) {
			if (map.texture) {
				SDL_DestroyTexture(map.texture);
			}
			map.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, w, h);
			map.w = w, map.h = h;
			if (map.texture) {
				SDL_SetTextureScaleMode(map.texture, SDL_SCALEMODE_LINEAR);
				SDL_SetTextureBlendMode(map.texture, s_light.lit);
			}
		}
		if (map.texture) {
			SDL_Texture* was = SDL_GetRenderTarget(renderer);
			uint8_t cr, cg, cb, ca;
			SDL_GetRenderDrawColor(renderer, &cr, &cg, &cb, &ca);
			SDL_SetRenderTarget(renderer, map.texture);
			SDL_SetRenderDrawColor(renderer, 128, 128, 128, 255);
			SDL_RenderClear(renderer);
			Place light = screen;
			light.x0 = 0, light.y0 = 0;
			light.kx = static_cast<float>(w) / screen.cam_w, light.ky = static_cast<float>(h) / screen.cam_h;
			SDL_SetTextureBlendMode(s_light.texture, SDL_BLENDMODE_NONE);
			SDL_SetTextureColorModFloat(s_light.texture, HALF, HALF, HALF);
			draw_cells(renderer, light);
			draw_lights(renderer, light, seconds, sun, fire, 1.f, shown);
			draw_ripples(renderer, light, seconds, 0.17f * water_light);
			draw_rays(renderer, light, seconds, above, 0.12f * water_light, 0.6f, 0.85f, 1.f);
			SDL_SetRenderTarget(renderer, was);
			SDL_SetRenderDrawColor(renderer, cr, cg, cb, ca);
			const SDL_FRect to{view.x, view.y, view.width, view.height};
			SDL_RenderTexture(renderer, map.texture, nullptr, &to);
		}
	} else {
		// without light maps, the world is only shaded
		SDL_SetTextureBlendMode(s_light.texture, SDL_BLENDMODE_MUL);
		SDL_SetTextureColorModFloat(s_light.texture, 1, 1, 1);
		draw_cells(renderer, screen);
	}

	// what glows, over the lit world: the haze of the shafts of light, the lamps, the eggs,
	// the hand, fireflies and the night sky
	draw_rays(renderer, screen, seconds, above, 0.30f * water_light, 0.72f, 0.9f, 1.f);
	draw_glowing(renderer, screen, seconds, sun, fire, shown);
	draw_night_sky(renderer, screen, seconds, now, night, main);
	SDL_SetRenderDrawBlendMode(renderer, draw_blend);
	if (!main) {
		return;
	}

	// where the sun is (or the moon, at night): a soft glow at the edge of the view
	const float cx = view.x + view.width / 2, cy = view.y + view.height / 2;
	auto at_edge = [&](float dx, float dy, float& x, float& y) {
		const float kx = std::abs(dx) > 1e-4f ? (view.width / 2) / std::abs(dx) : 1e9f;
		const float ky = std::abs(dy) > 1e-4f ? (view.height / 2) / std::abs(dy) : 1e9f;
		const float k = std::min(kx, ky);
		x = cx + dx * k;
		y = cy + dy * k;
	};
	float x, y;
	if (sun.strength > 0.02f) {
		// below the horizon at twilight, the glow stays on the horizon
		at_edge(sun.dx, std::min(sun.dy, -0.05f), x, y);
		const float radius = view.height * lerp(0.5f, 0.75f, sun.warm);
		glow_at(renderer, s_light.glow, x, y, radius, radius, 1.f, lerp(0.94f, 0.58f, sun.warm), lerp(0.80f, 0.30f, sun.warm), lerp(0.12f, 0.55f, sun.warm) * sun.strength);
	} else if (moon.strength > 0) {
		at_edge(moon.dx, moon.dy, x, y);
		glow_at(renderer, s_light.glow, x, y, view.height * 0.45f, view.height * 0.45f, 0.55f, 0.66f, 1.f, 0.14f * moon.strength);
	}
}

} // namespace

void render(SDL_Renderer* renderer) {
	if (ready(renderer)) {
		light_view(renderer, g_engine_context.viewport->get_main_camera_rect(), g_engine_context.viewport->get_screen_dest_rect(), nullptr, true);
	}
}

void render_view(SDL_Renderer* renderer, SDL_Texture* onto, const Rect2i& world, int width, int height, const void* slot) {
	if (!ready(renderer)) {
		return;
	}
	SDL_Texture* was = SDL_GetRenderTarget(renderer);
	SDL_SetRenderTarget(renderer, onto);
	light_view(renderer, world, Rect2f{0, 0, static_cast<float>(width), static_cast<float>(height)}, slot, false);
	SDL_SetRenderTarget(renderer, was);
}

} // namespace DayNight
