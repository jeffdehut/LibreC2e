#include "FileDialogs.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include "CrashReport.h"
#include "sdlbackend/SDLBackend.h"

#include <SDL3/SDL.h>
#include <memory>
#include <mutex>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace {

std::mutex s_mutex;
std::vector<std::function<void()>> s_finished; // run on the main thread by poll()

void finish(std::function<void()> f) {
	std::lock_guard<std::mutex> lock(s_mutex);
	s_finished.push_back(std::move(f));
}

SDL_Window* main_window() {
	auto* backend = dynamic_cast<SDLBackend*>(get_backend());
	return backend ? backend->window : nullptr;
}

// SDL needs the filters and strings until the dialog is closed
struct Request {
	std::vector<SDL_DialogFileFilter> filters;
	std::vector<std::string> names, patterns;
	std::string title, location;
	FileDialogs::Done done;
};

void SDLCALL dialog_closed(void* userdata, const char* const* files, int) {
	std::unique_ptr<Request> request(static_cast<Request*>(userdata));
	std::vector<std::string> paths;
	for (int i = 0; files && files[i]; ++i) {
		paths.emplace_back(files[i]);
	}
	auto done = std::move(request->done);
	finish([done = std::move(done), paths = std::move(paths)] { CrashReport::end_native_wait(); done(paths); });
}

void show(SDL_FileDialogType type, const std::string& title, const std::vector<FileDialogs::Filter>& filters, const std::string& location, bool many, FileDialogs::Done done) {
	auto* request = new Request;
	request->title = title;
	request->location = location;
	request->done = std::move(done);
	for (const auto& f : filters) {
		request->names.emplace_back(f.name);
		request->patterns.emplace_back(f.pattern);
	}
	for (size_t i = 0; i < filters.size(); ++i) {
		request->filters.push_back({request->names[i].c_str(), request->patterns[i].c_str()});
	}
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, request->filters.data());
	SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, static_cast<Sint64>(request->filters.size()));
	SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, main_window());
	if (!request->location.empty()) {
		SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, request->location.c_str());
	}
	SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, many);
	SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, request->title.c_str());
	CrashReport::begin_native_wait();
	SDL_ShowFileDialogWithProperties(type, dialog_closed, request, props);
	SDL_DestroyProperties(props);
}

} // namespace

namespace FileDialogs {

void open(const std::string& title, const std::vector<Filter>& filters, const std::string& start_in, bool many, Done done) {
	show(SDL_FILEDIALOG_OPENFILE, title, filters, start_in, many, std::move(done));
}

void save(const std::string& title, const std::vector<Filter>& filters, const std::string& default_path, Done done) {
	show(SDL_FILEDIALOG_SAVEFILE, title, filters, default_path, false, std::move(done));
}

void poll() {
	std::vector<std::function<void()>> ready;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		ready.swap(s_finished);
	}
	for (auto& f : ready) {
		f();
	}
}

} // namespace FileDialogs

// --- dragging a file out of the window ---

#if defined(__APPLE__) && TARGET_OS_OSX

extern "C" int oc1_mac_drag_start(void* ns_window, const char* path, void (*done)(int written, const char* error, void* context), void* context);

namespace DragOut {

bool supported() {
	return true;
}

void start(const std::string& path, std::function<void(Result)> done) {
	void* ns_window = SDL_GetPointerProperty(SDL_GetWindowProperties(main_window()), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
	auto* context = new std::function<void(Result)>(std::move(done));
	auto ended = [](int written, const char* error, void* c) {
		std::unique_ptr<std::function<void(Result)>> callback(static_cast<std::function<void(Result)>*>(c));
		auto f = std::move(*callback);
		const Result result{written != 0, error ? error : ""};
		finish([f = std::move(f), result] { f(result); });
	};
	if (!ns_window || !oc1_mac_drag_start(ns_window, path.c_str(), ended, context)) {
		ended(0, "Could not start the Finder export. The Norn was kept.", context);
	}
}

} // namespace DragOut

#elif defined(_WIN32)

namespace {

class DropSource : public IDropSource {
  public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
		if (riid == IID_IUnknown || riid == IID_IDropSource) {
			*out = static_cast<IDropSource*>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
	ULONG STDMETHODCALLTYPE Release() override {
		const LONG left = InterlockedDecrement(&refs);
		if (left == 0) {
			delete this;
		}
		return static_cast<ULONG>(left);
	}
	HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
		if (escape) {
			return DRAGDROP_S_CANCEL;
		}
		// dropped when the button that started it (either) is let go
		if (!(keys & (MK_LBUTTON | MK_RBUTTON))) {
			return DRAGDROP_S_DROP;
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

  private:
	LONG refs = 1;
};

std::wstring widen(const std::string& s) {
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	w.resize(w.size() - 1);
	return w;
}

} // namespace

namespace DragOut {

bool supported() {
	return true;
}

void start(const std::string& path, std::function<void(Result)> done) {
	static bool ole_ready = SUCCEEDED(OleInitialize(nullptr));
	bool dropped = false;
	IShellItem* item = nullptr;
	if (ole_ready && SUCCEEDED(SHCreateItemFromParsingName(widen(path).c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&item)))) {
		IDataObject* data = nullptr;
		if (SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_IDataObject, reinterpret_cast<void**>(&data)))) {
			auto* source = new DropSource;
			DWORD effect = DROPEFFECT_NONE;
			// a loop of its own until the drag ends; the game waits meanwhile
			const HRESULT result = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
			dropped = result == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
			source->Release();
			data->Release();
		}
		item->Release();
	}
	finish([done = std::move(done), dropped] { done(Result{dropped, ""}); });
}

} // namespace DragOut

#else

namespace DragOut {

bool supported() {
	return false;
}

void start(const std::string&, std::function<void(Result)> done) {
	finish([done = std::move(done)] { done(Result{}); });
}

} // namespace DragOut

#endif
