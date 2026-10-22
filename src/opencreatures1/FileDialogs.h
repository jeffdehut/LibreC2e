#pragma once

#include <functional>
#include <string>
#include <vector>

// The system's Open and Save dialogs, through SDL3. A dialog reports back when the player
// closes it, possibly on another thread, so the answer is handed back on the main thread by
// poll(), called once a frame.
namespace FileDialogs {

struct Filter {
	const char* name; // e.g. "Export Files (*.exp)"
	const char* pattern; // e.g. "exp", or "*" for all files
};

// The chosen paths, empty if the dialog was cancelled.
using Done = std::function<void(const std::vector<std::string>& paths)>;

void open(const std::string& title, const std::vector<Filter>& filters, const std::string& start_in, bool many, Done done);
// default_path is a folder, or a folder and file name to suggest
void save(const std::string& title, const std::vector<Filter>& filters, const std::string& default_path, Done done);

// Runs the callbacks of dialogs that have finished. Call on the main thread.
void poll();

} // namespace FileDialogs

// Dragging a file out of the window onto a folder or the desktop (the modern interface's
// export gesture). SDL can receive dropped files but not start a drag, so this is each
// system's own code: macOS and Windows; not Linux yet.
namespace DragOut {

bool supported();
// Starts dragging the file at path, while a mouse button is held. done is called (on
// the main thread, from poll()) with saved=true after a destination write on macOS, or
// after the native drop result on Windows; saved=false if the
// drag was cancelled or failed. The source file must survive until completion.
struct Result {
	bool saved = false;
	std::string error;
};
void start(const std::string& path, std::function<void(Result)> done);

} // namespace DragOut
