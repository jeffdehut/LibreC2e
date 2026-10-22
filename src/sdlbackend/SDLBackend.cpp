/*
 *  SDLBackend.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Sun Oct 24 2004.
 *  Copyright (c) 2004 Alyssa Milburn. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 */

#include "SDLBackend.h"
#include "SDLTouch.h"

#include <cstdlib>
#include <deque>

#include "common/Exception.h"
#include "common/NumericCast.h"
#include "common/backend/Keycodes.h"
#include "common/creaturesImage.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <array>
#include <cassert>
#include <imgui.h>
#include <memory>

static SDL_Window* s_main_window = nullptr; // for the pointer: see update_pointer_visibility

Backend* SDLBackend::get_instance() {
	static SDLBackend s_sdl_backend;
	return &s_sdl_backend;
}

SDLBackend::SDLBackend()
	: mainrendertarget(std::make_shared<SDLRenderTarget>(this)) {
	mainrendertarget->texture = nullptr;
}

void sdl_imgui_rebuild_font_texture(bool smooth);

static void ImGuiInit(SDL_Window* window, SDL_Renderer* renderer) {
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	io.IniFilename = nullptr; // don't save settings

	if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
		throw Exception("Couldn't initialize ImGui SDL3");
	}
	if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
		throw Exception("Couldn't initialize ImGui SDLRenderer");
	}

	io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines;
	sdl_imgui_rebuild_font_texture(false);
}

void sdl_imgui_rebuild_font_texture(bool smooth) {
	ImGuiIO& io = ImGui::GetIO();
	// fix for imgui#4768
	{
		ImGui_ImplSDLRenderer3_DestroyFontsTexture();
		auto* bd = ImGui_ImplSDLRenderer3_GetBackendData();
		unsigned char* pixels;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		bd->FontTexture = SDL_CreateTexture(bd->Renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, width, height);
		if (bd->FontTexture == nullptr) {
			throw Exception("Couldn't create ImGui font texture");
		}
		SDL_UpdateTexture(bd->FontTexture, nullptr, pixels, 4 * width);
		SDL_SetTextureBlendMode(bd->FontTexture, SDL_BLENDMODE_BLEND);
		SDL_SetTextureScaleMode(bd->FontTexture, smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
		io.Fonts->SetTexID((ImTextureID)(intptr_t)bd->FontTexture);
	}
}

static bool s_vsync = false;

void SDLBackend::init(const std::string& name, int32_t width, int32_t height) {
	// touch screens: SDLTouch makes the mouse actions, not SDL's emulation
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		throw Exception(std::string("SDL error during initialization: ") + SDL_GetError());
	}

	window = SDL_CreateWindow(name.c_str(),
		width, height,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!window) {
		throw Exception(std::string("SDL error creating window: ") + SDL_GetError());
	}

	renderer = SDL_CreateRenderer(window, nullptr);
	if (!renderer) {
		throw Exception(std::string("SDL error creating renderer: ") + SDL_GetError());
	}
	// draw in step with the display, so scrolling doesn't tear (run() falls back to its own
	// timing if this isn't available)
	s_vsync = SDL_SetRenderVSync(renderer, 1);
	fmt::print("* SDL vsync: {}\n", s_vsync ? "on" : fmt::format("off ({})", SDL_GetError()));

	SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
	fmt::print("* SDL Renderer: name={} max_texture_size={}x{} preferred_format={}\n",
		SDL_GetStringProperty(props, SDL_PROP_RENDERER_NAME_STRING, "unknown"),
		SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, -1),
		SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, -1),
		SDL_GetPixelFormatName(((const SDL_PixelFormat*)SDL_GetPointerProperty(props, SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, nullptr))[0]));

	SDL_HideCursor();
	s_main_window = window;
	sdl_touch_init(renderer, window);
#if !defined(SDL_PLATFORM_IOS) && !defined(SDL_PLATFORM_ANDROID)
	// on phones and tablets this would keep the system keyboard up: see sdl_want_keyboard
	SDL_StartTextInput(window);
#endif

	ImGuiInit(window, renderer);
}

void SDLBackend::shutdown() {
	SDL_Quit();
}

// The system pointer: shown over the interface (and always, when the game asks, e.g. while
// paused), hidden over the world, where the hand is the pointer. Only this code shows or hides
// it, so the two can't get out of step.
static bool s_pointer_always_visible = false;
static bool imguiHasCursorPrev = false;

static void update_pointer_visibility() {
	auto& io = ImGui::GetIO();
	// over another of our windows (classic kit windows), that window decides
	SDL_Window* focus = SDL_GetMouseFocus();
	if (focus && s_main_window && focus != s_main_window) {
		return;
	}
	const bool wanted = io.WantCaptureMouse || io.WantCaptureKeyboard || s_pointer_always_visible;
	// compared with the pointer's actual state, not a remembered one, so nothing that shows or
	// hides it elsewhere can leave it stuck hidden over the interface
	if (wanted != SDL_CursorVisible() || wanted != imguiHasCursorPrev) {
		if (wanted) {
			io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
			SDL_ShowCursor();
		} else {
			io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
			SDL_HideCursor();
		}
		imguiHasCursorPrev = wanted;
	}
}

void sdl_want_keyboard(bool wanted) {
#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_ANDROID)
	if (wanted) {
		SDL_StartTextInput(s_main_window);
	} else {
		SDL_StopTextInput(s_main_window);
	}
#else
	// on a computer, typed characters are always wanted (an interface text field may have
	// stopped them)
	(void)wanted;
	if (!SDL_TextInputActive(s_main_window)) {
		SDL_StartTextInput(s_main_window);
	}
#endif
}

void sdl_set_pointer_always_visible(bool visible) {
	s_pointer_always_visible = visible;
	update_pointer_visibility();
}

static bool ImGuiConsumeEvent(const SDL_Event& event) {
	ImGui_ImplSDL3_ProcessEvent(&event);

	auto& io = ImGui::GetIO();
	update_pointer_visibility();

	if (io.WantCaptureMouse && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL)) {
		return true;
	}
	if (io.WantCaptureKeyboard && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT)) {
		return true;
	}

	return false;
}

static bool should_quit = false;
static std::function<void(const SDL_Event&)> s_other_window_handler;

void sdl_set_other_window_event_handler(std::function<void(const SDL_Event&)> handler) {
	s_other_window_handler = std::move(handler);
}

// the window an event is for, or 0
static SDL_WindowID event_window(const SDL_Event& event) {
	if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) {
		return event.window.windowID;
	}
	switch (event.type) {
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP: return event.key.windowID;
		case SDL_EVENT_TEXT_INPUT: return event.text.windowID;
		case SDL_EVENT_TEXT_EDITING: return event.edit.windowID;
		case SDL_EVENT_MOUSE_MOTION: return event.motion.windowID;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: return event.button.windowID;
		case SDL_EVENT_MOUSE_WHEEL: return event.wheel.windowID;
		case SDL_EVENT_DROP_FILE:
		case SDL_EVENT_DROP_TEXT:
		case SDL_EVENT_DROP_BEGIN:
		case SDL_EVENT_DROP_COMPLETE:
		case SDL_EVENT_DROP_POSITION: return event.drop.windowID;
		default: return 0;
	}
}

bool SDLBackend::pollEvent(BackendEvent& e) {
	static std::deque<BackendEvent> s_gestures; // made from touches, given out first
	static std::vector<BackendEvent> s_touch_out;
	static bool s_in_batch = false; // pollEvent is called until it runs out: once a frame
	SDL_Event event;
	if (!s_in_batch) {
		s_in_batch = true;
		s_touch_out.clear();
		sdl_touch_poll(s_touch_out);
		for (auto& g : s_touch_out) {
			g.y -= static_cast<float>(mainrendertarget->viewport_offset_top);
			s_gestures.push_back(g);
		}
	}
retry:
	if (!s_gestures.empty()) {
		e = s_gestures.front();
		s_gestures.pop_front();
		return true;
	}
	if (!SDL_PollEvent(&event)) {
		s_in_batch = false;
		return false;
	}

	if (const SDL_WindowID id = event_window(event); id != 0 && id != SDL_GetWindowID(window)) {
		if (s_other_window_handler) {
			s_other_window_handler(event);
		}
		goto retry;
	}
	// closing the main window quits, even while other windows are open
	if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
		should_quit = true;
	}

	if (!SDL_ConvertEventToRenderCoordinates(renderer, &event)) {
		throw Exception(fmt::format("error converting event to render coordinates: {}", SDL_GetError()));
	}

	s_touch_out.clear();
	if (sdl_touch_handle(event, s_touch_out)) {
		for (auto& g : s_touch_out) {
			g.y -= static_cast<float>(mainrendertarget->viewport_offset_top);
			s_gestures.push_back(g);
		}
		goto retry;
	}

	if (ImGuiConsumeEvent(event)) {
		goto retry;
	}

	switch (event.type) {
		case SDL_EVENT_WINDOW_RESIZED:
			e.window_id = event.window.windowID;
			e.type = eventresizewindow;
			e.x = event.window.data1;
			e.y = event.window.data2;
			return true;

		case SDL_EVENT_MOUSE_MOTION:
			e.window_id = event.motion.windowID;
			e.type = eventmousemove;
			e.x = event.motion.x;
			e.y = event.motion.y - mainrendertarget->viewport_offset_top;
			e.xrel = event.motion.xrel;
			e.yrel = event.motion.yrel;
			e.button = 0;
			if (event.motion.state & SDL_BUTTON_MASK(1))
				e.button |= buttonleft;
			if (event.motion.state & SDL_BUTTON_MASK(2))
				e.button |= buttonmiddle;
			if (event.motion.state & SDL_BUTTON_MASK(3))
				e.button |= buttonright;
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			e.window_id = event.button.windowID;
			if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
				e.type = eventmousebuttondown;
			else
				e.type = eventmousebuttonup;
			switch (event.button.button) {
				case SDL_BUTTON_LEFT: e.button = buttonleft; break;
				case SDL_BUTTON_RIGHT: e.button = buttonright; break;
				case SDL_BUTTON_MIDDLE: e.button = buttonmiddle; break;
				default: goto retry;
			}
			e.x = event.button.x;
			e.y = event.button.y - mainrendertarget->viewport_offset_top;
			break;

		case SDL_EVENT_MOUSE_WHEEL: {
			e.window_id = event.wheel.windowID;
			e.type = eventmousebuttondown;
			e.wheel = true;
			const float flip = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.f : 1.f;
			e.wheel_x = event.wheel.x * flip;
			e.wheel_y = event.wheel.y * flip;
			e.x = event.wheel.mouse_x;
			e.y = event.wheel.mouse_y - mainrendertarget->viewport_offset_top;
			const SDL_Keymod mods = SDL_GetModState();
			e.key = ((mods & SDL_KMOD_CTRL) ? 1 : 0) | ((mods & SDL_KMOD_SHIFT) ? 2 : 0);
			if (event.wheel.y > 0) {
				e.button = buttonwheeldown;
			} else if (event.wheel.y < 0) {
				e.button = buttonwheelup;
			} else if (event.wheel.x != 0) {
				e.button = 0; // sideways only
			} else {
				goto retry;
			}
			break;
		}

		case SDL_EVENT_PINCH_UPDATE: {
			// a trackpad pinch zooms about the pointer, or the interface view under it
			if (ImGui::GetIO().WantCaptureMouse) {
				sdl_add_interface_zoom(event.pinch.scale);
				goto retry;
			}
			e.window_id = event.pinch.windowID;
			e.type = eventzoom;
			e.wheel_x = event.pinch.scale;
			float mx = 0, my = 0;
			SDL_GetMouseState(&mx, &my);
			SDL_RenderCoordinatesFromWindow(renderer, mx, my, &e.x, &e.y);
			e.y -= mainrendertarget->viewport_offset_top;
			break;
		}

		case SDL_EVENT_TEXT_INPUT:
			e.window_id = event.text.windowID;
			e.type = eventtextinput;
			e.text = event.text.text;
			break;

		case SDL_EVENT_KEY_UP: {
			e.window_id = event.key.windowID;
			int key = translateScancode(event.key.scancode);
			if (key != -1) {
				e.type = eventrawkeyup;
				e.key = key;
				return true;
			}
			goto retry;
		}

		case SDL_EVENT_KEY_DOWN: {
			e.window_id = event.key.windowID;
			int key = translateScancode(event.key.scancode);
			if (key != -1) {
				e.type = eventrawkeydown;
				e.key = key;
				return true;
			}
			goto retry;
		}

		case SDL_EVENT_QUIT:
			e.type = eventquit;
			break;

		default:
			goto retry;
	}

	return true;
}

SDLRenderTarget::SDLRenderTarget(SDLBackend* parent_)
	: parent(parent_) {
}

SDLRenderTarget::~SDLRenderTarget() {
	if (texture != nullptr) {
		printf("* SDL destroyed rendertarget texture %p\n", texture);
		SDL_DestroyTexture(texture);
	}
}

void SDLRenderTarget::renderLine(float x1, float y1, float x2, float y2, Color color) {
	SDL_SetRenderTarget(parent->renderer, texture);
	SDL_SetRenderDrawColor(parent->renderer, color.r, color.g, color.b, color.a);
	SDL_RenderLine(parent->renderer, x1, y1 + viewport_offset_top, x2, y2 + viewport_offset_top);
}

Texture SDLBackend::createTexture(int32_t width, int32_t height) {
	// TODO: make sure we get an alpha-capable texture format. seems to always
	// happen by default, but should check just in case.
	Texture tex(
		SDL_CreateTexture(renderer, SDL_PIXELFORMAT_UNKNOWN, SDL_TEXTUREACCESS_STATIC, width, height),
		width, height, &SDL_DestroyTexture);
	if (!tex) {
		throw Exception(fmt::format("error creating texture: {}", SDL_GetError()));
	}
	// enable alpha blending
	SDL_SetTextureBlendMode(tex.as<SDL_Texture*>(), SDL_BLENDMODE_BLEND);
	// nearest pixel sampling for source textures
	SDL_SetTextureScaleMode(tex.as<SDL_Texture*>(), SDL_SCALEMODE_NEAREST);
	return tex;
}

struct SDLSurfaceDeleter {
	void operator()(SDL_Surface* ptr) {
		SDL_DestroySurface(ptr);
	}
};

void SDLBackend::updateTexture(Texture& tex, Rect2i location, const Image& image) {
	assert(tex);
	assert(image.data);
	assert(image.width > 0);
	assert(image.height > 0);
	if (image.format != if_index8) {
		assert(image.palette.data() == nullptr);
	}

	// map our format enum to SDL's format enum
	SDL_PixelFormat sdlformat = SDL_PIXELFORMAT_UNKNOWN;
	switch (image.format) {
		case if_index8:
			sdlformat = SDL_PIXELFORMAT_INDEX8;
			break;
		case if_rgb555:
			sdlformat = SDL_PIXELFORMAT_XRGB1555;
			break;
		case if_rgb565:
			sdlformat = SDL_PIXELFORMAT_RGB565;
			break;
		case if_bgr24:
			sdlformat = SDL_PIXELFORMAT_BGR24;
			break;
		case if_rgb24:
			sdlformat = SDL_PIXELFORMAT_RGB24;
			break;
	}

	// create initial surface
	std::unique_ptr<SDL_Surface, SDLSurfaceDeleter> surf{SDL_CreateSurfaceFrom(
		image.width,
		image.height,
		sdlformat,
		const_cast<uint8_t*>(image.data.data()),
		image.width * SDL_BYTESPERPIXEL(sdlformat) // pitch
		)};
	assert(surf);

	// set palette
	if (image.format == if_index8) {
		if (!image.palette) {
			throw Exception("Got indexed image without palette");
		}
		shared_array<SDL_Color> palette(image.palette.size());
		for (size_t i = 0; i < image.palette.size(); ++i) {
			palette[i].r = image.palette[i].r;
			palette[i].g = image.palette[i].g;
			palette[i].b = image.palette[i].b;
			palette[i].a = image.palette[i].a;
		}
		SDL_SetPaletteColors(SDL_CreateSurfacePalette(surf.get()), palette.data(), 0, palette.size());
	}

	// set colour-keying
	if (image.colorkey.a > 0) {
		if (image.colorkey.a != 255) {
			throw Exception("Expected alpha value of transparent color to be 255");
		}
		const Uint32 sdlcolorkey = SDL_MapRGB(
			SDL_GetPixelFormatDetails(surf->format),
			SDL_GetSurfacePalette(surf.get()),
			image.colorkey.r,
			image.colorkey.g,
			image.colorkey.b);
		SDL_SetSurfaceColorKey(surf.get(), true, sdlcolorkey);
	}

	// convert surface to texture format
	std::unique_ptr<SDL_Surface, SDLSurfaceDeleter> converted{
		SDL_ConvertSurface(surf.get(), tex.as<SDL_Texture*>()->format)};
	assert(converted);

	// update texture
	SDL_Rect rect;
	rect.x = location.x;
	rect.y = location.y;
	rect.w = location.width;
	rect.h = location.height;
	if (!SDL_UpdateTexture(tex.as<SDL_Texture*>(), location == Rect2i{} ? nullptr : &rect,
			converted->pixels, converted->pitch)) {
		throw Exception(fmt::format("error updating texture: {}", SDL_GetError()));
	};
}

int32_t SDLRenderTarget::getWidth() const {
	if (texture) {
		float width;
		SDL_GetTextureSize(texture, &width, nullptr);
		return numeric_cast<int32_t>(width);
	} else {
		int width;
		SDL_GetWindowSizeInPixels(parent->window, &width, nullptr);
		width = width / SDL_GetWindowDisplayScale(parent->window);
		return numeric_cast<int32_t>(width);
	}
}

int32_t SDLRenderTarget::getHeight() const {
	if (texture) {
		float height;
		SDL_GetTextureSize(texture, nullptr, &height);
		assert(viewport_offset_top == 0);
		assert(viewport_offset_bottom == 0);
		return numeric_cast<int32_t>(height);
	} else {
		int height;
		SDL_GetWindowSizeInPixels(parent->window, nullptr, &height);
		height = height / SDL_GetWindowDisplayScale(parent->window);
		return numeric_cast<int32_t>(height - viewport_offset_top - viewport_offset_bottom);
	}
}

void SDLRenderTarget::setClip(Rect2f dest) {
	// TODO: SDL_RenderSetClipRectF doesn't seem to exist?
	SDL_Rect clip;
	clip.x = dest.x;
	clip.y = dest.y;
	clip.w = dest.width;
	clip.h = dest.height;
	if (!SDL_SetRenderClipRect(parent->renderer, dest == Rect2f{} ? nullptr : &clip)) {
		throw Exception(fmt::format("error in SDL_RenderSetClipRect(): {}", SDL_GetError()));
	}
}

void SDLRenderTarget::setViewportOffsetTop(int offset_top) {
	viewport_offset_top = offset_top;
}

void SDLRenderTarget::setViewportOffsetBottom(int offset_bottom) {
	viewport_offset_bottom = offset_bottom;
}

void SDLRenderTarget::renderTexture(const Texture& tex_, Rect2i src, Rect2f dest, RenderOptions options) {
	if (dest.right() + tex_.width <= 0 || dest.x >= getWidth() || dest.bottom() <= 0 || dest.y >= getHeight()) {
		// cull non-visible textures
		return;
	}

	SDL_Texture* tex = const_cast<Texture&>(tex_).as<SDL_Texture*>();
	assert(tex);

	SDL_SetTextureAlphaMod(tex, options.alpha);
	SDL_FlipMode flip = options.mirror ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;

	SDL_FRect srcrect;
	srcrect.x = src.x;
	srcrect.y = src.y;
	srcrect.w = src.width;
	srcrect.h = src.height;

	SDL_FRect destrect;
	destrect.x = dest.x;
	destrect.y = dest.y;
	destrect.w = (options.override_drawsize ? options.overridden_drawwidth : dest.width) * options.scale;
	destrect.h = (options.override_drawsize ? options.overridden_drawheight : dest.height) * options.scale;

	SDL_SetRenderTarget(parent->renderer, texture);
	SDL_RenderTextureRotated(parent->renderer, tex, &srcrect, &destrect, 0, nullptr, flip);
}

void SDLRenderTarget::renderCreaturesImage(creaturesImage& img, unsigned int frame, int x, int y, RenderOptions options) {
	if (x + numeric_cast<int>(img.width(frame)) <= 0 || x >= numeric_cast<int>(getWidth()) || y + numeric_cast<int>(img.height(frame)) <= 0 || y >= numeric_cast<int>(getHeight())) {
		return;
	}

	if (!img.getTextureForFrame(frame)) {
		img.getTextureForFrame(frame) = parent->createTextureFromImage(img.getImageForFrame(frame));
	}

	Rect2i src;
	src.x = img.getXOffsetForFrame(frame);
	src.y = img.getYOffsetForFrame(frame);
	src.width = img.width(frame);
	src.height = img.height(frame);

	Rect2f dest;
	dest.x = x;
	dest.y = y + viewport_offset_top;
	dest.width = (options.override_drawsize ? options.overridden_drawwidth : img.width(frame)) * options.scale;
	dest.height = (options.override_drawsize ? options.overridden_drawheight : img.height(frame)) * options.scale;

	return renderTexture(img.getTextureForFrame(frame), src, dest, options);
}

void SDLRenderTarget::renderClear() {
	SDL_SetRenderDrawColor(parent->renderer, 0, 0, 0, 255);
	SDL_SetRenderTarget(parent->renderer, texture);
	SDL_RenderClear(parent->renderer);
}

void SDLRenderTarget::blitRenderTarget(RenderTarget* s, Rect2f dest) {
	SDLRenderTarget* src = dynamic_cast<SDLRenderTarget*>(s);
	assert(src);

	SDL_FRect r;
	r.x = dest.x;
	r.y = dest.y + viewport_offset_top;
	r.w = dest.width;
	r.h = dest.height;
	SDL_SetRenderTarget(parent->renderer, texture);
	SDL_RenderTexture(parent->renderer, src->texture, nullptr, &r);
}

RenderTarget* SDLBackend::getMainRenderTarget() {
	return mainrendertarget.get();
}

std::shared_ptr<RenderTarget> SDLBackend::newRenderTarget(int32_t w, int32_t h) {
	SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_UNKNOWN, SDL_TEXTUREACCESS_TARGET, w, h);
	assert(texture);
	printf("* SDL created rendertarget texture %p\n", texture);

	auto newtarget = std::make_shared<SDLRenderTarget>(this);
	newtarget->texture = texture;
	return std::dynamic_pointer_cast<RenderTarget>(newtarget);
}

// left out: menu, select, execute, snapshot, numeric keypad
struct _keytrans {
	int sdl;
	Openc2eKeycode openc2e;
};
static const std::array<_keytrans, 72> keytrans = {{
	{SDL_SCANCODE_BACKSPACE, OPENC2E_KEY_BACKSPACE},
	{SDL_SCANCODE_TAB, OPENC2E_KEY_TAB},
	{SDL_SCANCODE_CLEAR, OPENC2E_KEY_CLEAR},
	{SDL_SCANCODE_RETURN, OPENC2E_KEY_RETURN},
	{SDL_SCANCODE_RSHIFT, OPENC2E_KEY_SHIFT},
	{SDL_SCANCODE_LSHIFT, OPENC2E_KEY_SHIFT},
	{SDL_SCANCODE_RCTRL, OPENC2E_KEY_CTRL},
	{SDL_SCANCODE_LCTRL, OPENC2E_KEY_CTRL},
	{SDL_SCANCODE_PAUSE, OPENC2E_KEY_PAUSE},
	{SDL_SCANCODE_CAPSLOCK, OPENC2E_KEY_CAPSLOCK},
	{SDL_SCANCODE_ESCAPE, OPENC2E_KEY_ESCAPE},
	{SDL_SCANCODE_SPACE, OPENC2E_KEY_SPACE},
	{SDL_SCANCODE_PAGEUP, OPENC2E_KEY_PAGEUP},
	{SDL_SCANCODE_PAGEDOWN, OPENC2E_KEY_PAGEDOWN},
	{SDL_SCANCODE_END, OPENC2E_KEY_END},
	{SDL_SCANCODE_HOME, OPENC2E_KEY_HOME},
	{SDL_SCANCODE_LEFT, OPENC2E_KEY_LEFT},
	{SDL_SCANCODE_UP, OPENC2E_KEY_UP},
	{SDL_SCANCODE_RIGHT, OPENC2E_KEY_RIGHT},
	{SDL_SCANCODE_DOWN, OPENC2E_KEY_DOWN},
	{SDL_SCANCODE_PRINTSCREEN, OPENC2E_KEY_PRINTSCREEN},
	{SDL_SCANCODE_INSERT, OPENC2E_KEY_INSERT},
	{SDL_SCANCODE_DELETE, OPENC2E_KEY_DELETE},
	{SDL_SCANCODE_0, OPENC2E_KEY_0},
	{SDL_SCANCODE_1, OPENC2E_KEY_1},
	{SDL_SCANCODE_2, OPENC2E_KEY_2},
	{SDL_SCANCODE_3, OPENC2E_KEY_3},
	{SDL_SCANCODE_4, OPENC2E_KEY_4},
	{SDL_SCANCODE_5, OPENC2E_KEY_5},
	{SDL_SCANCODE_6, OPENC2E_KEY_6},
	{SDL_SCANCODE_7, OPENC2E_KEY_7},
	{SDL_SCANCODE_8, OPENC2E_KEY_8},
	{SDL_SCANCODE_9, OPENC2E_KEY_9},
	{SDL_SCANCODE_A, OPENC2E_KEY_A},
	{SDL_SCANCODE_B, OPENC2E_KEY_B},
	{SDL_SCANCODE_C, OPENC2E_KEY_C},
	{SDL_SCANCODE_D, OPENC2E_KEY_D},
	{SDL_SCANCODE_E, OPENC2E_KEY_E},
	{SDL_SCANCODE_F, OPENC2E_KEY_F},
	{SDL_SCANCODE_G, OPENC2E_KEY_G},
	{SDL_SCANCODE_H, OPENC2E_KEY_H},
	{SDL_SCANCODE_I, OPENC2E_KEY_I},
	{SDL_SCANCODE_J, OPENC2E_KEY_J},
	{SDL_SCANCODE_K, OPENC2E_KEY_K},
	{SDL_SCANCODE_L, OPENC2E_KEY_L},
	{SDL_SCANCODE_M, OPENC2E_KEY_M},
	{SDL_SCANCODE_N, OPENC2E_KEY_N},
	{SDL_SCANCODE_O, OPENC2E_KEY_O},
	{SDL_SCANCODE_P, OPENC2E_KEY_P},
	{SDL_SCANCODE_Q, OPENC2E_KEY_Q},
	{SDL_SCANCODE_R, OPENC2E_KEY_R},
	{SDL_SCANCODE_S, OPENC2E_KEY_S},
	{SDL_SCANCODE_T, OPENC2E_KEY_T},
	{SDL_SCANCODE_U, OPENC2E_KEY_U},
	{SDL_SCANCODE_V, OPENC2E_KEY_V},
	{SDL_SCANCODE_W, OPENC2E_KEY_W},
	{SDL_SCANCODE_X, OPENC2E_KEY_X},
	{SDL_SCANCODE_Y, OPENC2E_KEY_Y},
	{SDL_SCANCODE_Z, OPENC2E_KEY_Z},
	{SDL_SCANCODE_F1, OPENC2E_KEY_F1},
	{SDL_SCANCODE_F2, OPENC2E_KEY_F2},
	{SDL_SCANCODE_F3, OPENC2E_KEY_F3},
	{SDL_SCANCODE_F4, OPENC2E_KEY_F4},
	{SDL_SCANCODE_F5, OPENC2E_KEY_F5},
	{SDL_SCANCODE_F6, OPENC2E_KEY_F6},
	{SDL_SCANCODE_F7, OPENC2E_KEY_F7},
	{SDL_SCANCODE_F8, OPENC2E_KEY_F8},
	{SDL_SCANCODE_F9, OPENC2E_KEY_F9},
	{SDL_SCANCODE_F10, OPENC2E_KEY_F10},
	{SDL_SCANCODE_F11, OPENC2E_KEY_F11},
	{SDL_SCANCODE_F12, OPENC2E_KEY_F12},
	{SDL_SCANCODE_NUMLOCKCLEAR, OPENC2E_KEY_NUMLOCK},
}};

int SDLBackend::translateScancode(int key) {
	for (auto keytran : keytrans) {
		if (keytran.sdl == key)
			return keytran.openc2e;
	}

	return -1;
}

// TODO: this is possibly not a great idea, we should maybe maintain our own state table
bool SDLBackend::keyDown(Openc2eKeycode key) {
	const bool* keystate = SDL_GetKeyboardState(nullptr);

	for (auto keytran : keytrans) {
		if (keytran.openc2e == key)
			if (keystate[keytran.sdl])
				return true;
	}

	return false;
}

static constexpr int OPENC2E_MAX_FPS = 60;
static constexpr int OPENC2E_MIN_FPS = 20;


bool sdl_event_watcher(void* userdata, SDL_Event* event) {
	(void)userdata;
	if (event->type == SDL_EVENT_QUIT) {
		should_quit = true;
	}
	return true;
}

void SDLBackend::run(std::function<bool()> update_callback) {
	SDL_AddEventWatch(sdl_event_watcher, nullptr);

	uint32_t last_frame_end = 0;

	while (true) {
		// TODO: we have to calculate renderer sizes when the backend is initialized,
		// otherwise side panels get in weird locations. related to issue with panels
		// when resizing in general?

		// TODO: does this work on Windows?
		SDL_SetRenderScale(renderer, SDL_GetWindowDisplayScale(window), SDL_GetWindowDisplayScale(window));

		bool focused = SDL_GetWindowFlags(window) & (SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
		Uint32 desired_ticks_per_frame = 1000 / (focused ? OPENC2E_MAX_FPS : OPENC2E_MIN_FPS);
		Uint32 frame_end = SDL_GetTicks();
		// with vsync the display paces the frames while focused; in the background (or
		// without vsync) a frame cap saves power
		if ((!s_vsync || !focused) && frame_end - last_frame_end < desired_ticks_per_frame) {
			SDL_Delay(desired_ticks_per_frame - (frame_end - last_frame_end));
		}
		last_frame_end = SDL_GetTicks();

		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();

		bool result = update_callback();
		if (should_quit || !result) {
			break;
		}

		ImGui::Render();
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

		// debugging: OPENC2E_SCREENSHOT=<file.bmp> saves frame OPENC2E_SCREENSHOT_FRAME (default 100)
		static int frame_number = 0;
		if (const char* shot = std::getenv("OPENC2E_SCREENSHOT")) {
			const char* at = std::getenv("OPENC2E_SCREENSHOT_FRAME");
			if (++frame_number == (at ? std::atoi(at) : 100)) {
				if (SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr)) {
					SDL_SaveBMP(surface, shot);
					SDL_DestroySurface(surface);
				}
			}
		}

		SDL_RenderPresent(renderer);
	}
}