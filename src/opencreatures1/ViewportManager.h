#pragma once

#include "common/math/Rect.h"

#include <stdint.h>

struct BackendEvent;

static constexpr int CREATURES1_WORLD_WIDTH = 8352;
static constexpr int CREATURES1_WORLD_HEIGHT = 1200;

class ViewportManager {
  public:
	ViewportManager() = default;
	void handle_event(const BackendEvent& event);
	// Every frame, with the real time since the last one: arrow keys, glides, a fling after
	// dragging, and easing in the steps Track Norn makes each tick. Time-based, so it's smooth
	// at any frame rate, the same at every game speed, and works while paused (not in the
	// original, which only had scroll bars: batch 10, Q4).
	void frame(float seconds);
	// zooms by factor about a window point, between min_scale and MAX_SCALE (pinch, Ctrl+wheel,
	// Shift+wheel when shift_wheel_zooms); never so far out that the world stops filling the
	// view from top to bottom
	void zoom_at(float factor, float winx, float winy);
	static constexpr float MAX_SCALE = 3.0f;
	float min_scale = 1.0f;
	bool shift_wheel_zooms = false;
	// pans by a distance in window pixels (two-finger drag, trackpad scrolling)
	void pan_by_window(float dx, float dy);
	bool dragging() const { return middle_dragging; }

	float window_x_to_world_x(float) const;
	float window_y_to_world_y(float) const;

	const Rect2i& get_main_camera_rect() const;
	const Rect2f& get_screen_dest_rect() const;

	void set_scroll_position(int32_t scrollx, int32_t scrolly);
	// centres the view on a world point, gliding there when smooth scrolling is on
	void centre_on(float x, float y);
	float world_x_to_window_x(float) const;
	float world_y_to_window_y(float) const;

	// The camera options (batch 3, Q2 and Q4): magnification (Toggle size), smooth scrolling,
	// and infinite scroll, which lifts the limits at the top and bottom of the world.
	float scale = 1.0f;
	bool smooth_scrolling = true;
	bool infinite_scroll = false;

	void set_margin_top(float);
	void set_margin_bottom(float);

	// whether the player is scrolling by hand right now (the arrow keys)
	bool manually_scrolling() const { return scroll_left || scroll_right || scroll_up || scroll_down; }
	// moves the view, wrapping round the world; eased moves are spread over the next frames
	void scroll_by(int32_t dx, int32_t dy, bool eased = false);

	int32_t get_scrollx() const { return scrollx; }
	int32_t get_scrolly() const { return scrolly; }

	// recalculates the camera after its position or the window changes
	void update();

  private:

	// arrow keys, per 90 ms tick of real time (not in the original)
	static constexpr float SCROLL_ACCEL = 8;
	static constexpr float SCROLL_DECEL = 0.5;
	static constexpr float SCROLL_MAX = 64;
	static constexpr float TICK_SECONDS = 0.09f;

	bool scroll_left = false;
	bool scroll_right = false;
	bool scroll_up = false;
	bool scroll_down = false;

	// the exact position; scrollx and scrolly are it rounded
	float fx = 0;
	float fy = 0;
	int32_t scrollx = 0;
	int32_t scrolly = 0;

	float scroll_velx = 0;
	float scroll_vely = 0;

	// where a smooth scroll is gliding to, and how fast
	bool gliding = false;
	float glide_speed = 0;
	int32_t glide_x = 0;
	int32_t glide_y = 0;

	// distance still to cover from eased moves
	float pending_x = 0;
	float pending_y = 0;

	// middle-button dragging, and the fling after it, in world px per second
	bool middle_dragging = false;
	float drag_last_x = 0;
	float drag_last_y = 0;
	double drag_last_time = 0;
	float fling_vx = 0;
	float fling_vy = 0;

	float margin_top = 0;
	float margin_bottom = 0;

	Rect2i main_camera;
	Rect2f screen_dest;
};
