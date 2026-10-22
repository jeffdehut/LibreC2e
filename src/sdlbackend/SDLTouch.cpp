#include "SDLTouch.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <SDL3/SDL.h>
#include <cmath>
#include <map>
#include <utility>

// Touch screens (tablets, phones, touch laptops). SDL's own touch-to-mouse emulation is
// turned off, and this turns finger events into what the game understands:
//
// - over the interface, a finger is the mouse with its left button held: tap a button, drag
//   a slider, scroll a list;
// - over the world, a quick tap is a left click (use, tickle, slap) and touch-and-hold is a
//   right click (pick up, drop). After a hold, dragging moves the hand, so a held object goes
//   with the finger, and lifting the finger lets it go there;
// - a double tap on the world starts typing to the Norns and brings up the system keyboard;
// - two fingers drag the world and pinch to zoom.
//
// Trackpads also report fingers; only touch screens are handled here (trackpads already
// scroll and pinch through the wheel and pinch events).

namespace {

constexpr float MOVE_SLOP = 14; // render pixels a finger may wander and still be a tap or hold
constexpr Uint64 HOLD_MS = 450;
constexpr Uint64 DOUBLE_TAP_MS = 350;
constexpr float DOUBLE_TAP_SLOP = 40;

enum class Mode {
	Idle,
	Interface, // the finger is the mouse with the left button down
	World, // waiting to see whether it's a tap, a hold or a move
	WorldHeld, // the hold's right click has been sent
	WorldMoving, // the finger moves the hand without clicking
	TwoFingers,
};

SDL_Renderer* s_renderer = nullptr;
SDL_WindowID s_window = 0;
std::function<void(float, float)> s_double_tap;

Mode s_mode = Mode::Idle;
SDL_FingerID s_first = 0;
std::map<SDL_FingerID, std::pair<float, float>> s_fingers; // render coordinates
float s_start_x = 0, s_start_y = 0;
float s_last_x = 0, s_last_y = 0;
Uint64 s_start_ms = 0;
bool s_moved_after_hold = false;
// the last tap, for double taps
Uint64 s_tap_ms = 0;
float s_tap_x = 0, s_tap_y = 0;
// two fingers
float s_pair_cx = 0, s_pair_cy = 0, s_pair_distance = 0;
float s_interface_zoom = 1;
float s_interface_distance = 0; // two fingers on the interface

void to_window(float rx, float ry, float& wx, float& wy) {
	wx = rx;
	wy = ry;
	if (s_renderer) {
		SDL_RenderCoordinatesToWindow(s_renderer, rx, ry, &wx, &wy);
	}
}

void push_motion(float rx, float ry) {
	SDL_Event e{};
	e.type = SDL_EVENT_MOUSE_MOTION;
	e.motion.windowID = s_window;
	e.motion.which = SDL_TOUCH_MOUSEID;
	to_window(rx, ry, e.motion.x, e.motion.y);
	SDL_PushEvent(&e);
}

void push_button(Uint8 button, bool down, float rx, float ry) {
	SDL_Event e{};
	e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
	e.button.windowID = s_window;
	e.button.which = SDL_TOUCH_MOUSEID;
	e.button.button = button;
	e.button.down = down;
	e.button.clicks = 1;
	to_window(rx, ry, e.button.x, e.button.y);
	SDL_PushEvent(&e);
}

void push_click(Uint8 button, float rx, float ry) {
	push_button(button, true, rx, ry);
	push_button(button, false, rx, ry);
}

bool over_interface(float rx, float ry) {
	if (!ImGui::GetCurrentContext()) {
		return false;
	}
	ImGuiWindow* hovered = nullptr;
	ImGuiWindow* under_moving = nullptr;
	ImGui::FindHoveredWindowEx(ImVec2(rx, ry), false, &hovered, &under_moving);
	return hovered != nullptr;
}

void pair_measure(float& cx, float& cy, float& distance) {
	auto a = s_fingers.begin()->second;
	auto b = std::next(s_fingers.begin())->second;
	cx = (a.first + b.first) / 2;
	cy = (a.second + b.second) / 2;
	distance = std::hypot(a.first - b.first, a.second - b.second);
}

bool is_touch_screen(SDL_TouchID id) {
	const SDL_TouchDeviceType type = SDL_GetTouchDeviceType(id);
	return type != SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE && type != SDL_TOUCH_DEVICE_INDIRECT_RELATIVE;
}

} // namespace

void sdl_touch_init(SDL_Renderer* renderer, SDL_Window* window) {
	s_renderer = renderer;
	s_window = SDL_GetWindowID(window);
}

void sdl_set_touch_double_tap_handler(std::function<void(float x, float y)> handler) {
	s_double_tap = std::move(handler);
}

float sdl_take_interface_zoom() {
	const float z = s_interface_zoom;
	s_interface_zoom = 1;
	return z;
}

void sdl_add_interface_zoom(float factor) {
	if (factor > 0) {
		s_interface_zoom *= factor;
	}
}

void sdl_touch_poll(std::vector<BackendEvent>& out) {
	// two fingers on the interface pinch it
	if (s_mode == Mode::Interface && s_fingers.size() >= 2) {
		float cx, cy, distance;
		pair_measure(cx, cy, distance);
		if (s_interface_distance > 1 && distance > 1) {
			s_interface_zoom *= distance / s_interface_distance;
		}
		s_interface_distance = distance;
	} else {
		s_interface_distance = 0;
	}
	if (s_mode == Mode::TwoFingers && s_fingers.size() >= 2) {
		float cx, cy, distance;
		pair_measure(cx, cy, distance);
		if (cx != s_pair_cx || cy != s_pair_cy) {
			BackendEvent pan{};
			pan.window_id = s_window;
			pan.type = eventpan;
			pan.wheel_x = s_pair_cx - cx;
			pan.wheel_y = s_pair_cy - cy;
			out.push_back(pan);
		}
		if (s_pair_distance > 1 && distance > 1 && distance != s_pair_distance) {
			BackendEvent zoom{};
			zoom.window_id = s_window;
			zoom.type = eventzoom;
			zoom.x = cx;
			zoom.y = cy;
			zoom.wheel_x = distance / s_pair_distance;
			out.push_back(zoom);
		}
		s_pair_cx = cx;
		s_pair_cy = cy;
		s_pair_distance = distance;
	}
	// a finger held still long enough over the world is a right click
	if (s_mode == Mode::World && SDL_GetTicks() - s_start_ms >= HOLD_MS) {
		push_click(SDL_BUTTON_RIGHT, s_start_x, s_start_y);
		s_mode = Mode::WorldHeld;
		s_moved_after_hold = false;
	}
}

bool sdl_touch_handle(const SDL_Event& event, std::vector<BackendEvent>& out) {
	if (event.type != SDL_EVENT_FINGER_DOWN && event.type != SDL_EVENT_FINGER_MOTION && event.type != SDL_EVENT_FINGER_UP && event.type != SDL_EVENT_FINGER_CANCELED) {
		return false;
	}
	if (!is_touch_screen(event.tfinger.touchID)) {
		return true; // trackpad fingers: their scrolling and pinching come as other events
	}
	const SDL_FingerID id = event.tfinger.fingerID;
	const float x = event.tfinger.x, y = event.tfinger.y; // already in render coordinates

	if (event.type == SDL_EVENT_FINGER_DOWN) {
		s_fingers[id] = {x, y};
		if (s_fingers.size() == 1) {
			s_first = id;
			s_start_x = s_last_x = x;
			s_start_y = s_last_y = y;
			s_start_ms = SDL_GetTicks();
			push_motion(x, y);
			if (over_interface(x, y)) {
				s_mode = Mode::Interface;
				push_button(SDL_BUTTON_LEFT, true, x, y);
			} else {
				s_mode = Mode::World;
			}
		} else if (s_fingers.size() == 2 && s_mode != Mode::Interface) {
			// a second finger on the world: drag and pinch instead (a pending tap or hold
			// is dropped; a hold that already picked something up keeps it)
			s_mode = Mode::TwoFingers;
			pair_measure(s_pair_cx, s_pair_cy, s_pair_distance);
		}
		return true;
	}

	if (event.type == SDL_EVENT_FINGER_MOTION) {
		if (!s_fingers.count(id)) {
			return true;
		}
		s_fingers[id] = {x, y};
		switch (s_mode) {
			case Mode::Interface:
				if (id == s_first) {
					push_motion(x, y);
				}
				break;
			case Mode::World:
				if (id == s_first && std::hypot(x - s_start_x, y - s_start_y) > MOVE_SLOP) {
					s_mode = Mode::WorldMoving;
					push_motion(x, y);
				}
				break;
			case Mode::WorldHeld:
			case Mode::WorldMoving:
				if (id == s_first) {
					if (std::hypot(x - s_start_x, y - s_start_y) > MOVE_SLOP) {
						s_moved_after_hold = true;
					}
					push_motion(x, y);
				}
				break;
			case Mode::TwoFingers:
				// measured once a frame, in sdl_touch_poll: fingers report one at a time, and a
				// drag with both would look like a pinch in between
				break;
			case Mode::Idle:
				break;
		}
		s_last_x = x;
		s_last_y = y;
		return true;
	}

	// a finger lifted (or the system took the touch away)
	const bool canceled = event.type == SDL_EVENT_FINGER_CANCELED;
	s_fingers.erase(id);
	if (s_mode == Mode::TwoFingers) {
		if (s_fingers.empty()) {
			s_mode = Mode::Idle;
		} else if (s_fingers.size() >= 2) {
			pair_measure(s_pair_cx, s_pair_cy, s_pair_distance);
		}
		return true;
	}
	if (id != s_first) {
		return true;
	}
	switch (s_mode) {
		case Mode::Interface:
			push_button(SDL_BUTTON_LEFT, false, x, y);
			break;
		case Mode::World:
			if (canceled) {
				break;
			}
			if (SDL_GetTicks() - s_tap_ms <= DOUBLE_TAP_MS && std::hypot(x - s_tap_x, y - s_tap_y) <= DOUBLE_TAP_SLOP) {
				s_tap_ms = 0;
				if (s_double_tap) {
					s_double_tap(x, y);
				}
			} else {
				push_click(SDL_BUTTON_LEFT, x, y);
				s_tap_ms = SDL_GetTicks();
				s_tap_x = x;
				s_tap_y = y;
			}
			break;
		case Mode::WorldHeld:
			// held, then dragged: let go of what the hold picked up, where the finger lifted
			if (s_moved_after_hold && !canceled) {
				push_click(SDL_BUTTON_RIGHT, x, y);
			}
			break;
		default:
			break;
	}
	s_mode = s_fingers.empty() ? Mode::Idle : s_mode;
	if (s_mode != Mode::Idle && s_mode != Mode::TwoFingers) {
		s_mode = Mode::Idle;
	}
	return true;
}
