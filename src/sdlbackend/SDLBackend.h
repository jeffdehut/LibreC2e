/*
 *  SDLBackend.h
 *  openc2e
 *
 *  Created by Alyssa Milburn on Sun Oct 24 2004.
 *  Copyright (c) 2004-2006 Alyssa Milburn. All rights reserved.
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

#pragma once

#include <functional>

#include "common/backend/Backend.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <array>
#include <memory>
#include <string>

class SDLBackend;

class SDLRenderTarget : public RenderTarget {
  public:
	SDLBackend* parent;
	SDL_Texture* texture = nullptr;
	int viewport_offset_top = 0;
	int viewport_offset_bottom = 0;

	SDLRenderTarget(SDLBackend* parent);
	SDLRenderTarget(const SDLRenderTarget&) = delete;
	SDLRenderTarget(SDLRenderTarget&&) = delete;
	SDLRenderTarget& operator=(const SDLRenderTarget&) = delete;
	SDLRenderTarget& operator=(SDLRenderTarget&&) = delete;
	~SDLRenderTarget();

	void renderTexture(const Texture& tex, Rect2i src, Rect2f dest, RenderOptions options) override;
	void renderCreaturesImage(creaturesImage& tex, unsigned int frame, int x, int y, RenderOptions options) override;
	void renderLine(float x1, float y1, float x2, float y2, Color color) override;
	void blitRenderTarget(RenderTarget* src, Rect2f dest) override;

	// dimensions in logical window points (device-independent pixels), not pixels
	int32_t getWidth() const override;
	int32_t getHeight() const override;
	void setClip(Rect2f dest) override;

	void renderClear() override;
	void setViewportOffsetTop(int offset_top) override;
	void setViewportOffsetBottom(int offset_bottom) override;
};

class SDLBackend : public Backend {
	friend class SDLRenderTarget;

  protected:
	SDL_Renderer* renderer = nullptr;
	std::shared_ptr<SDLRenderTarget> mainrendertarget;

	SDLBackend();
	SDLBackend(const SDLBackend&) = delete;
	SDLBackend(SDLBackend&&) = delete;
	SDLBackend& operator=(const SDLBackend&) = delete;
	SDLBackend& operator=(SDLBackend&&) = delete;

	int translateScancode(int key);

  public:
	SDL_Window* window = nullptr;

	static Backend* get_instance();
	void init(const std::string& name, int32_t width, int32_t height) override;
	void run(std::function<bool()> render_callback) override;
	void shutdown() override;

	bool pollEvent(BackendEvent& e) override;

	Texture createTexture(int32_t width, int32_t height) override;
	void updateTexture(Texture& tex, Rect2i location, const Image& image) override;

	RenderTarget* getMainRenderTarget() override;
	std::shared_ptr<RenderTarget> newRenderTarget(int32_t width, int32_t height) override;

	bool keyDown(Openc2eKeycode key) override;
};

// Rebuilds ImGui's font texture after fonts were added to its atlas. smooth uses linear
// filtering, for fonts rasterised at a higher resolution than they're shown.
void sdl_imgui_rebuild_font_texture(bool smooth);
// Events for windows other than the main one (opencreatures1's classic kit windows) go to
// this handler instead of the game.
// shows the system pointer everywhere (e.g. while the game is paused), not only over the
// interface
void sdl_set_pointer_always_visible(bool visible);
// Typing is wanted (or not): shows or hides the system keyboard on phones and tablets.
void sdl_want_keyboard(bool wanted);
void sdl_set_other_window_event_handler(std::function<void(const union SDL_Event&)> handler);
