#pragma once

#include <stdint.h>
#include <string>

enum eventtype {
	eventquit,
	eventtextinput,
	eventrawkeyup,
	eventrawkeydown,
	eventmousebuttondown,
	eventmousebuttonup,
	eventmousemove,
	eventresizewindow,
	// not from SDL directly: wheel_x is the zoom factor, (x, y) the point to zoom about
	eventzoom,
	// not from SDL directly: move the view by (wheel_x, wheel_y) window pixels
	eventpan,
};
enum eventbuttons { buttonleft = 0x1,
	buttonright = 0x2,
	buttonmiddle = 0x4,
	buttonwheeldown = 0x8,
	buttonwheelup = 0x10 };

struct BackendEvent {
	uint32_t window_id;
	eventtype type;
	float x, y;
	int xrel, yrel;
	int key;
	unsigned int button;
	std::string text;
	// a mouse wheel or trackpad scroll: set with the scroll amounts (key holds 1 for Ctrl, 2
	// for Shift); the event is a press of buttonwheelup or buttonwheeldown, or of no button
	// for a sideways-only scroll
	bool wheel = false;
	float wheel_x = 0, wheel_y = 0;
};