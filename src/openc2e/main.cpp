/*
 *  main.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Wed 02 Jun 2004.
 *  Copyright (c) 2004-2008 Alyssa Milburn. All rights reserved.
 *  Copyright (c) 2005-2008 Bryan Donlan. All rights reserved.
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

#include "Engine.h"
#include "common/backtrace.h"
#include "common/Exception.h"
#include "sdlbackend/SDLBackend.h"
#include "sdlbackend/SDLMixerBackend.h"
#include "version.h"

#include <fmt/core.h>
#include <memory>
#include <chrono>
#include <cstdlib>
#include "World.h"

#ifdef _WIN32
#include <windows.h>
#endif

// SDL tries stealing main on some platforms, which we don't want.
#undef main

int main(int argc, char* argv[]) {
	install_backtrace_printer();

	std::string version;
#ifdef DEV_BUILD
	version = "development build";
#else
	version = RELEASE_VERSION;
#endif
	fmt::print("openc2e ({}), built " __DATE__ " " __TIME__ "\nCopyright (c) 2004-2008 Alyssa Milburn and others\n\n", version);

	engine.addPossibleBackend("sdl", SDLBackend::get_instance());
	engine.addPossibleAudioBackend("sdlmixer", SDLMixerBackend::get_instance());

	// pass command-line flags to the engine, but do no other setup
	try {
		if (!engine.parseCommandLine(argc, argv))
			return 1;
	} catch (const Exception& e) {
		fmt::print(stderr, "error: {}\n", e.what());
		return 1;
	}

	try {
	// get the engine to do all the startup (read catalogue, loading world, etc)
	if (!engine.initialSetup())
		return 0;

	const auto start = std::chrono::steady_clock::now();
	const char* duration_env = std::getenv("OPENC2E_SESSION_DURATION_MS");
	const auto duration = duration_env ? std::strtoll(duration_env, nullptr, 10) : 0;
	unsigned frames = 0;
	const bool check_pause = std::getenv("OPENC2E_SESSION_TEST_PAUSE") != nullptr;
	const auto initial_tick = world.tickcount;
	bool resumed = false;
	if (check_pause) world.paused = true;
	get_backend()->run([&] {
		if (check_pause && !resumed && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(300)) {
			if (world.tickcount != initial_tick) throw Exception("C2 paused world advanced");
			world.paused = false;
			resumed = true;
		}
		engine.tick();
		if (engine.done || (duration > 0 && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(duration))) {
			return false;
		}
		engine.drawWorld();
		++frames;
		return true;
	});

	if (check_pause) {
		if (!resumed || world.tickcount == initial_tick) throw Exception("C2 world did not resume");
		fmt::print("TEST C2 pause/resume PASS: {} -> {}\n", initial_tick, world.tickcount);
	}
	fmt::print("SESSION C2: object_section_parsed=yes frames={} simulation_ticks={} saving=unsupported external_kits=unsupported\n", frames, world.tickcount);

	// we're done, be sure to shut stuff down
	engine.shutdown();

	return 0;
	} catch (const std::exception& e) {
		fmt::print(stderr, "World startup/runtime failed: {}\n", e.what());
		return 1;
	}
}

/* vim: set noet: */
