#include "ViewportManager.h"

#include "C1SoundManager.h"
#include "EngineContext.h"
#include "common/NumericCast.h"
#include "common/backend/Backend.h"
#include "common/backend/BackendEvent.h"
#include "common/backend/Keycodes.h"
#include "common/math/ModularArithmetic.h"
#include "common/render/RenderSystem.h"

#include <algorithm>
#include <chrono>
#include <math.h>

namespace {
double now_seconds() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

void ViewportManager::handle_event(const BackendEvent& event) {
	// middle-button drag: the world follows the mouse, and keeps going a little when let go
	if (event.type == eventmousebuttondown && event.button == buttonmiddle) {
		middle_dragging = true;
		drag_last_x = event.x;
		drag_last_y = event.y;
		drag_last_time = now_seconds();
		fling_vx = fling_vy = 0;
		gliding = false;
		return;
	}
	if (event.type == eventmousebuttonup && event.button == buttonmiddle) {
		middle_dragging = false;
		// no fling if the mouse had stopped before the button was let go
		if (now_seconds() - drag_last_time > 0.08) {
			fling_vx = fling_vy = 0;
		}
		return;
	}
	if (event.type == eventmousemove && middle_dragging && !(event.button & buttonmiddle)) {
		middle_dragging = false; // let go somewhere the view didn't hear about (the interface)
		fling_vx = fling_vy = 0;
	}
	if (event.type == eventmousemove && middle_dragging) {
		const double t = now_seconds();
		const float per_px = screen_dest.width > 0 ? static_cast<float>(main_camera.width) / screen_dest.width : 1;
		const float dx = (event.x - drag_last_x) * per_px;
		const float dy = (event.y - drag_last_y) * per_px;
		fx -= dx;
		fy -= dy;
		const float dt = static_cast<float>(std::max(t - drag_last_time, 1.0 / 240));
		// smoothed, so one jittery sample doesn't decide the fling
		fling_vx = fling_vx * 0.5f + (-dx / dt) * 0.5f;
		fling_vy = fling_vy * 0.5f + (-dy / dt) * 0.5f;
		drag_last_x = event.x;
		drag_last_y = event.y;
		drag_last_time = t;
		update();
		return;
	}
	// the wheel or a trackpad's two-finger scroll moves the view; Ctrl zooms. Shift turns a
	// plain mouse wheel sideways, since the world is far wider than it is tall.
	if (event.type == eventmousebuttondown && event.wheel) {
		if ((event.key & 1) || (shift_wheel_zooms && (event.key & 2))) { // Ctrl (or Shift)
			// (Shift turns some mice's wheel sideways)
			const float steps = event.wheel_y != 0 ? event.wheel_y : event.wheel_x;
			zoom_at(std::pow(1.1f, steps), event.x, event.y);
		} else if (event.key & 2) { // Shift
			pan_by_window(-event.wheel_y * 40, 0);
		} else {
			pan_by_window(event.wheel_x * 40, -event.wheel_y * 40);
		}
		return;
	}
	if (event.type == eventzoom) {
		zoom_at(event.wheel_x, event.x, event.y);
		return;
	}
	if (event.type == eventpan) {
		pan_by_window(event.wheel_x, event.wheel_y);
		return;
	}
	if (!(event.type == eventrawkeyup || event.type == eventrawkeydown)) {
		return;
	}
	if (event.key == OPENC2E_KEY_LEFT) {
		scroll_left = (event.type == eventrawkeydown);
	} else if (event.key == OPENC2E_KEY_RIGHT) {
		scroll_right = (event.type == eventrawkeydown);
	} else if (event.key == OPENC2E_KEY_UP) {
		scroll_up = (event.type == eventrawkeydown);
	} else if (event.key == OPENC2E_KEY_DOWN) {
		scroll_down = (event.type == eventrawkeydown);
	}
}

void ViewportManager::scroll_by(int32_t dx, int32_t dy, bool eased) {
	gliding = false;
	if (eased) {
		pending_x += static_cast<float>(dx);
		pending_y += static_cast<float>(dy);
		return;
	}
	fx += static_cast<float>(dx);
	fy += static_cast<float>(dy);
	update();
}

void ViewportManager::pan_by_window(float dx, float dy) {
	const float per_px = screen_dest.width > 0 ? static_cast<float>(main_camera.width) / screen_dest.width : 1;
	pending_x += dx * per_px;
	pending_y += dy * per_px;
	gliding = false;
	fling_vx = fling_vy = 0;
}

void ViewportManager::zoom_at(float factor, float winx, float winy) {
	// (a two-finger drag reports tiny changes in spread; they aren't a pinch)
	if (!(factor > 0) || std::abs(factor - 1) < 0.002f || screen_dest.width <= 0 || screen_dest.height <= 0) {
		return;
	}
	// out no further than the world's height fills the view
	const float fill = screen_dest.height / static_cast<float>(CREATURES1_WORLD_HEIGHT);
	const float lowest = min_scale < 1 ? std::min(1.f, std::max(min_scale, fill)) : min_scale;
	const float new_scale = std::clamp(scale * factor, lowest, MAX_SCALE);
	if (new_scale == scale) {
		return;
	}
	// keep the world point under (winx, winy) where it is
	const float rx = (winx - screen_dest.x) / screen_dest.width;
	const float ry = (winy - screen_dest.y) / screen_dest.height;
	const float wx = fx + rx * static_cast<float>(main_camera.width);
	const float wy = fy + ry * static_cast<float>(main_camera.height);
	scale = new_scale;
	update();
	fx = wx - rx * static_cast<float>(main_camera.width);
	fy = wy - ry * static_cast<float>(main_camera.height);
	gliding = false;
	update();
}

void ViewportManager::frame(float seconds) {
	seconds = std::clamp(seconds, 0.f, 0.1f); // after a stall, don't jump
	const float ticks = seconds / TICK_SECONDS;
	// arrow keys: the same feel as before, now per tick of real time
	auto key_axis = [&](float& vel, bool minus, bool plus) {
		if (minus) {
			vel -= SCROLL_ACCEL * ticks;
		}
		if (plus) {
			vel += SCROLL_ACCEL * ticks;
		}
		if (!minus && !plus) {
			vel *= std::pow(SCROLL_DECEL, ticks);
			if (std::abs(vel) < 0.1f) {
				vel = 0;
			}
		}
		vel = std::clamp(vel, -SCROLL_MAX, SCROLL_MAX);
	};
	key_axis(scroll_velx, scroll_left, scroll_right);
	key_axis(scroll_vely, scroll_up, scroll_down);
	fx += scroll_velx * ticks;
	fy += scroll_vely * ticks;
	if (scroll_velx != 0 || scroll_vely != 0) {
		gliding = false;
	}

	// the fling after a middle-button drag slows to a stop in about half a second
	if (!middle_dragging && (fling_vx != 0 || fling_vy != 0)) {
		fx += fling_vx * seconds;
		fy += fling_vy * seconds;
		const float keep = std::exp(-seconds * 6);
		fling_vx *= keep;
		fling_vy *= keep;
		if (std::abs(fling_vx) < 8 && std::abs(fling_vy) < 8) {
			fling_vx = fling_vy = 0;
		}
	}

	// eased moves (Track Norn's steps, the wheel, two-finger drags): most of the way within
	// about a tenth of a second
	if (pending_x != 0 || pending_y != 0) {
		const float part = 1 - std::exp(-seconds / 0.05f);
		float mx = pending_x * part, my = pending_y * part;
		if (std::abs(pending_x - mx) < 0.25f) {
			mx = pending_x;
		}
		if (std::abs(pending_y - my) < 0.25f) {
			my = pending_y;
		}
		fx += mx;
		fy += my;
		pending_x -= mx;
		pending_y -= my;
	}

	if (gliding) {
		// Batch 4, Q17: a smooth move changes speed by 2 px per tick, moves at least 4 px per
		// tick, and moves exactly the rest once that's under 3 px. It slows down in time to
		// stop. The short way round the world. Worked out per frame, in ticks of real time.
		float dx = static_cast<float>(glide_x) - fx;
		if (dx > CREATURES1_WORLD_WIDTH / 2) {
			dx -= CREATURES1_WORLD_WIDTH;
		} else if (dx < -CREATURES1_WORLD_WIDTH / 2) {
			dx += CREATURES1_WORLD_WIDTH;
		}
		const float dy = static_cast<float>(glide_y) - fy;
		const float distance = std::sqrt(dx * dx + dy * dy);
		if (distance < 3) {
			fx = static_cast<float>(glide_x);
			fy = static_cast<float>(glide_y);
			gliding = false;
			glide_speed = 0;
		} else {
			const float stopping = glide_speed * glide_speed / 4; // distance to slow down at 2 px/tick
			glide_speed = stopping >= distance ? glide_speed - 2 * ticks : glide_speed + 2 * ticks;
			glide_speed = std::min(std::max(glide_speed, 4.f), distance);
			const float move = std::min(glide_speed * ticks, distance);
			fx += dx / distance * move;
			fy += dy / distance * move;
		}
	}

	update();
}

void ViewportManager::update() {
	const float width = numeric_cast<float>(get_backend()->getMainRenderTarget()->getWidth());
	const float height = numeric_cast<float>(get_backend()->getMainRenderTarget()->getHeight());
	const int32_t viewport_height = numeric_cast<int32_t>((height - margin_top - margin_bottom) / scale);

	// clamp scroll
	// can't go past top or bottom
	if (infinite_scroll) {
		// no limits, but keep the world somewhere on screen
		fy = std::clamp(fy, static_cast<float>(-viewport_height + 32), static_cast<float>(CREATURES1_WORLD_HEIGHT - 32));
	} else if (fy < 0) {
		fy = 0;
		scroll_vely = 0;
		pending_y = 0;
		fling_vy = 0;
	} else if (static_cast<float>(CREATURES1_WORLD_HEIGHT) - fy < static_cast<float>(viewport_height)) {
		fy = static_cast<float>(std::max(0, CREATURES1_WORLD_HEIGHT - viewport_height));
		scroll_vely = 0;
		pending_y = 0;
		fling_vy = 0;
	}
	// wraparound left and right
	fx = mod_remainder(fx, static_cast<float>(CREATURES1_WORLD_WIDTH));
	scrollx = mod_remainder(static_cast<int32_t>(std::lround(fx)), CREATURES1_WORLD_WIDTH);
	scrolly = static_cast<int32_t>(std::lround(fy));

	// update rects and soundmanager
	// TODO: this doesn't feel like the best place for this
	main_camera = Rect2i{
		scrollx,
		scrolly,
		numeric_cast<int32_t>(width / scale),
		viewport_height};
	screen_dest = Rect2f{0, margin_top, width, height - margin_top - margin_bottom};
	g_engine_context.sounds->set_listener_position(main_camera);
}

const Rect2i& ViewportManager::get_main_camera_rect() const {
	return main_camera;
}

const Rect2f& ViewportManager::get_screen_dest_rect() const {
	return screen_dest;
}

float ViewportManager::window_x_to_world_x(float winx) const {
	// TODO: move this to RenderSystem?
	float worldx = (winx - screen_dest.x) / screen_dest.width * numeric_cast<float>(main_camera.width) + numeric_cast<float>(main_camera.x);
	worldx = mod_remainder(worldx, numeric_cast<float>(CREATURES1_WORLD_WIDTH));
	return worldx;
}

float ViewportManager::window_y_to_world_y(float winy) const {
	// TODO: move this to RenderSystem?
	return (winy - screen_dest.y) / screen_dest.height * numeric_cast<float>(main_camera.height) + numeric_cast<float>(main_camera.y);
}

void ViewportManager::set_scroll_position(int32_t scrollx_, int32_t scrolly_) {
	scrollx = scrollx_;
	scrolly = scrolly_;
	fx = static_cast<float>(scrollx_);
	fy = static_cast<float>(scrolly_);
	pending_x = pending_y = 0;
	fling_vx = fling_vy = 0;
	gliding = false;
}

void ViewportManager::centre_on(float x, float y) {
	// jumps are clamped to x 0 .. world width - view width, without wrapping (batch 4, Q17)
	const int32_t targetx = std::clamp(numeric_cast<int32_t>(x) - main_camera.width / 2, 0, std::max(0, CREATURES1_WORLD_WIDTH - main_camera.width));
	const int32_t targety = numeric_cast<int32_t>(y) - main_camera.height / 2;
	// glide only to somewhere within a screen; further away, jump
	const bool near = std::abs(targetx - scrollx) <= main_camera.width && std::abs(targety - scrolly) <= main_camera.height;
	if (smooth_scrolling && near) {
		gliding = true;
		glide_x = targetx;
		glide_y = targety;
	} else {
		set_scroll_position(targetx, targety);
		update();
	}
}

float ViewportManager::world_x_to_window_x(float worldx) const {
	float relx = worldx - numeric_cast<float>(main_camera.x);
	relx = mod_remainder(relx, numeric_cast<float>(CREATURES1_WORLD_WIDTH));
	return relx / numeric_cast<float>(main_camera.width) * screen_dest.width + screen_dest.x;
}

float ViewportManager::world_y_to_window_y(float worldy) const {
	return (worldy - numeric_cast<float>(main_camera.y)) / numeric_cast<float>(main_camera.height) * screen_dest.height + screen_dest.y;
}

void ViewportManager::set_margin_bottom(float margin_bottom_) {
	margin_bottom = margin_bottom_;
	update();
}
void ViewportManager::set_margin_top(float margin_top_) {
	margin_top = margin_top_;
	update();
}
