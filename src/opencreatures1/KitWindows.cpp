#include "KitWindows.h"

#include "UIImages.h"
#include "UIStyle.h"
#include "sdlbackend/SDLBackend.h"

#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <cstdlib>
#include <map>
#include <vector>

namespace KitWindows {

namespace {

struct KitWindow {
	SDL_Window* window = nullptr;
	SDL_Renderer* renderer = nullptr;
	ImGuiContext* context = nullptr;
	bool* open = nullptr;
	bool shown = false;
	bool on_top = false;
	std::string title;
};

std::map<int, KitWindow> s_windows;
bool s_handler_installed = false;

// runs f with a kit window's interface context current, then puts the main one back
template <typename F>
void in_context(KitWindow& w, F f) {
	ImGuiContext* previous = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(w.context);
	f();
	ImGui::SetCurrentContext(previous);
}

void handle_event(const SDL_Event& event) {
	SDL_Window* target = SDL_GetWindowFromEvent(&event);
	for (auto& [id, w] : s_windows) {
		if (w.window != target) {
			continue;
		}
		if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && w.open) {
			*w.open = false;
		}
		// the game hides the system pointer over its own window; kits use it
		if (event.type == SDL_EVENT_WINDOW_MOUSE_ENTER) {
			SDL_ShowCursor();
		} else if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
			SDL_HideCursor();
		}
		SDL_Event converted = event;
		SDL_ConvertEventToRenderCoordinates(w.renderer, &converted);
		in_context(w, [&] { ImGui_ImplSDL3_ProcessEvent(&converted); });
		return;
	}
}

void destroy(KitWindow& w) {
	in_context(w, [&] {
		ImGui_ImplSDLRenderer3_Shutdown();
		ImGui_ImplSDL3_Shutdown();
	});
	ImGui::DestroyContext(w.context);
	UIImages::forget_renderer(w.renderer);
	SDL_DestroyRenderer(w.renderer);
	SDL_DestroyWindow(w.window);
	SDL_HideCursor();
}

KitWindow* create(int id, const std::string& title) {
	if (!s_handler_installed) {
		sdl_set_other_window_event_handler(handle_event);
		s_handler_installed = true;
	}
	KitWindow w;
	if (!SDL_CreateWindowAndRenderer(title.c_str(), 540, 420, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &w.window, &w.renderer)) {
		return nullptr;
	}
    SDL_SetWindowMinimumSize(w.window, 420, 320);
	SDL_SetRenderVSync(w.renderer, 0);
	// cascade them over the game's window, like the original's kits opening one by one
	int gx = 0, gy = 0;
	int count = 0;
	SDL_Window** all = SDL_GetWindows(&count);
	if (all && count > 0) {
		SDL_GetWindowPosition(all[0], &gx, &gy);
	}
	SDL_free(all);
	SDL_SetWindowPosition(w.window, gx + 60 + 28 * (id % 8), gy + 80 + 24 * (id % 8));
	SDL_StartTextInput(w.window);

	ImGuiContext* previous = ImGui::GetCurrentContext();
	w.context = ImGui::CreateContext();
	ImGui::SetCurrentContext(w.context);
	ImGui::GetIO().IniFilename = nullptr;
	ImGui_ImplSDL3_InitForSDLRenderer(w.window, w.renderer);
	ImGui_ImplSDLRenderer3_Init(w.renderer);
	UIStyle::init(false);
	ImGui::SetCurrentContext(previous);
    SDL_ShowWindow(w.window);
    SDL_RaiseWindow(w.window);
	w.title = title;
	return &(s_windows[id] = w);
}

} // namespace

void show(int id, const std::string& title, bool* open, bool on_top, int fixed_width, int fixed_height, const std::function<void()>& body) {
	const bool fixed = fixed_width > 0 && fixed_height > 0;
	auto it = s_windows.find(id);
	KitWindow* w = it != s_windows.end() ? &it->second : create(id, title);
	if (!w) {
		return;
	}
	w->open = open;
	w->shown = true;
	if (fixed) {
		int ww = 0, wh = 0;
		SDL_GetWindowSize(w->window, &ww, &wh);
		if (ww != fixed_width || wh != fixed_height) {
			SDL_SetWindowResizable(w->window, false);
			SDL_SetWindowSize(w->window, fixed_width, fixed_height);
		}
	}
	if (w->title != title) {
		SDL_SetWindowTitle(w->window, title.c_str());
		w->title = title;
	}
	if (w->on_top != on_top) {
		SDL_SetWindowAlwaysOnTop(w->window, on_top);
		w->on_top = on_top;
	}

	const float scale = SDL_GetWindowDisplayScale(w->window);
	SDL_SetRenderScale(w->renderer, scale, scale);
	in_context(*w, [&] {
		UIImages::set_target_renderer(w->renderer);
		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		// the whole window is the kit
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoBringToFrontOnFocus;
		ImGui::Begin("##kit", nullptr, flags);
		body();
		ImGui::End();
		ImGui::Render();
		const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
		SDL_SetRenderDrawColorFloat(w->renderer, bg.x, bg.y, bg.z, 1);
		SDL_RenderClear(w->renderer);
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), w->renderer);
		// debugging: with OPENC2E_SCREENSHOT=<file.bmp>, each kit window is saved as
		// <file>-kit<id>.bmp after 120 frames
		if (const char* shot = std::getenv("OPENC2E_SCREENSHOT")) {
			static std::map<int, int> frames;
			if (++frames[id] == 120) {
				if (SDL_Surface* surface = SDL_RenderReadPixels(w->renderer, nullptr)) {
					SDL_SaveBMP(surface, (std::string(shot) + "-kit" + std::to_string(id) + ".bmp").c_str());
					SDL_DestroySurface(surface);
				}
			}
		}
		SDL_RenderPresent(w->renderer);
		UIImages::set_target_renderer(nullptr);
	});
}

// A closed window is hidden at once and destroyed about half a second later, once macOS has
// delivered the events it still had queued for it (a mouse-entered event for a destroyed
// window crashed in AppKit's tracking-area code).
struct Retiring {
	KitWindow window;
	int frames_left;
};
std::vector<Retiring> s_retiring;

void retire(KitWindow& w) {
	SDL_HideWindow(w.window);
	s_retiring.push_back({w, 30});
}

void destroy_retired(bool all) {
	for (auto it = s_retiring.begin(); it != s_retiring.end();) {
		if (all || --it->frames_left <= 0) {
			destroy(it->window);
			it = s_retiring.erase(it);
		} else {
			++it;
		}
	}
}

void end_frame() {
	for (auto it = s_windows.begin(); it != s_windows.end();) {
		if (!it->second.shown) {
			retire(it->second);
			it = s_windows.erase(it);
		} else {
			it->second.shown = false;
			++it;
		}
	}
	destroy_retired(false);
}

void close_all() {
	for (auto& [id, w] : s_windows) {
		retire(w);
	}
	s_windows.clear();
	destroy_retired(false);
}

} // namespace KitWindows
