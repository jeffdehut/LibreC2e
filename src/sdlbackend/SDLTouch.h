#pragma once

#include "common/backend/BackendEvent.h"

#include <functional>
#include <vector>

union SDL_Event;
struct SDL_Renderer;
struct SDL_Window;

// Touch-screen gestures for the main window (see SDLTouch.cpp).
void sdl_touch_init(SDL_Renderer* renderer, SDL_Window* window);
// Called before each batch of events: turns a long enough hold into a right click, and
// adds the frame's two-finger drag and pinch to out.
void sdl_touch_poll(std::vector<BackendEvent>& out);
// Takes a finger event (in render coordinates); returns false for other events. Gestures that
// aren't mouse actions (two-finger drags and pinches) are added to out.
bool sdl_touch_handle(const SDL_Event& event, std::vector<BackendEvent>& out);
// Called with the render-coordinate point of a double tap on the world.
void sdl_set_touch_double_tap_handler(std::function<void(float x, float y)> handler);
// Pinching over the interface (a trackpad, or two fingers on a touch screen): the zoom
// factor gathered since the last call (1 if none), for an interface view that zooms.
float sdl_take_interface_zoom();
void sdl_add_interface_zoom(float factor);
