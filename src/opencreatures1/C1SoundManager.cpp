#include "C1SoundManager.h"

#include "C1ControlledSound.h"
#include "EngineContext.h"
#include "PathManager.h"
#include "common/Exception.h"
#include "common/audio/AudioBackend.h"
#include "common/math/ModularArithmetic.h"

#include <assert.h>
#include <fmt/core.h>
#include <math.h>

namespace fs = std::filesystem;

constexpr bool SOUND_MANAGER_DEBUG = false;

C1SoundManager::C1SoundManager() {
}

C1SoundManager::~C1SoundManager() {
    // Uncontrolled sounds belong to this world too. Do not leave them playing
    // underneath the next world's sounds when the engine context is replaced.
    for (const auto& sound : data) if (sound.channel) get_audio_backend()->audio_channel_stop(sound.channel);
}

bool C1SoundManager::is_muted() {
	return muted;
}

void C1SoundManager::set_muted(bool muted_) {
	if (muted == muted_) {
		return; // called every frame; only touch the sounds when it changes
	}
	muted = muted_;
	update_volumes();
}

C1SoundManager::SoundData* C1SoundManager::get_sound_data(AudioChannel channel) {
	if (!channel) {
		return nullptr;
	}

	uint16_t index = channel.handle & 0xffff;

	if (index >= data.size()) {
		return nullptr;
	}
	SoundData* sounddata = &data[index];
	if (sounddata->channel != channel) {
		return nullptr;
	}

	return sounddata;
}

template <typename T>
T clamp(T value, T low, T high) {
	assert(!(high < low));
	return (value < low) ? low : (high < value) ? high
												: value;
}

struct DistanceInfo {
	float distx;
	float disty;
	float volume;
	float pan;
};

// Batch 3, Q24: sounds are placed by the source's offset from the view's centre, wrapping
// round the world. Units are hundredths of a decibel (likely), so -10000 is silent.
constexpr float PAN_RANGE = 10000;
constexpr float SILENT = -10000;

static DistanceInfo calculate_distance(Rect2f listener, float world_wrap_width, Rect2f sound) {
	const auto centerx = listener.x + listener.width / 2;
	const auto centery = listener.y + listener.height / 2;

	const float distx = mod_distance(
		sound.x + sound.width / 2.0f,
		centerx,
		world_wrap_width);
	const float disty = sound.y + sound.height / 2.0f - centery;

	const float screen_width = listener.width;
	const float screen_height = listener.height;

	// pan: 5000 x offset / view width, within +-10000
	const float pan = clamp(5000 * distx / screen_width, -PAN_RANGE, PAN_RANGE);
	// volume: full within the view, then down by 10000 x (distance past the edge / view
	// width), plus the same for height, down to silence
	const float past_x = std::max(0.f, fabsf(distx) - screen_width / 2);
	const float past_y = std::max(0.f, fabsf(disty) - screen_height / 2);
	const float attenuation = std::max(SILENT, -10000 * (past_x / screen_width) - 10000 * (past_y / screen_width));

	DistanceInfo d;
	d.distx = distx;
	d.disty = disty;
	d.volume = attenuation <= SILENT ? 0 : powf(10, attenuation / 2000);
	d.pan = pan / PAN_RANGE;
	return d;
}

void C1SoundManager::update_volume(SoundData& s) {
	if (get_audio_backend()->audio_channel_get_state(s.channel) != AUDIO_PLAYING) {
		return;
	}

	float volume = muted ? 0 : s.volume;

	if (s.position != Rect2f{}) {
		auto distance = calculate_distance(listener, numeric_cast<float>(world_wrap_width), s.position);
		volume *= distance.volume;
		get_audio_backend()->audio_channel_set_pan(s.channel, distance.pan);
	}

	get_audio_backend()->audio_channel_set_volume(s.channel, volume);
}

void C1SoundManager::update_volumes() {
	for (auto& s : data) {
		update_volume(s);
	}
}

void C1SoundManager::update() {
	update_volumes();
}

AudioChannel C1SoundManager::play_sound_helper(std::string name, Rect2f initial_position, bool loop) {
	if (name.size() == 0) {
		return {};
	}

	if (initial_position != Rect2f{}) {
		auto distance = calculate_distance(listener, numeric_cast<float>(world_wrap_width), initial_position);
		if (distance.volume <= 0) {
			if (loop) {
				// TODO: probably worth not loading looping sounds, or at least
				// pausing them, until they're audible. SDL_Mixer still applies
				// effects and mixes channels that have zero volume.
			} else {
				// SNDE and SNDC only play on or near the screen (batch 3, Q24)
				return {};
			}
		}
	}

	fs::path filename = g_engine_context.paths->find_path(PATH_TYPE_SOUND, name + ".wav");
	if (filename.empty()) {
		// creatures 1 ignores non-existent audio clips
		fmt::print("WARNING: couldn't find audio clip {:?}\n", name);
		return {};
	}

	auto channel = get_audio_backend()->play_clip(filename, loop);
	if (!channel) {
		// Not played: all 32 channels busy (batch 6, Q4: the original silently skips the sound)
		// or the clip couldn't be loaded. Never fatal; a game left running for days must not
		// stop over a sound.
		return {};
	}

	uint16_t index = channel.handle & 0xffff;
	if (index >= data.size()) {
		data.resize(index + 1);
	}
	data[index] = {};
	data[index].channel = channel;
	data[index].name = name;
	data[index].position = initial_position;
	data[index].looping = loop;
	update_volume(data[index]);
	return channel;
}

void C1SoundManager::play_uncontrolled_sound(std::string name, Rect2f initial_position) {
	play_sound_helper(name, initial_position, false);
}

C1ControlledSound C1SoundManager::play_controlled_sound(std::string name, Rect2f initial_position, bool loop) {
	return C1ControlledSound{play_sound_helper(name, initial_position, loop)};
}

void C1SoundManager::set_listener_position(Rect2f listener_) {
	if (SOUND_MANAGER_DEBUG) {
		fmt::print("C1SoundManager::set_listener_position\n");
		for (auto& s : data) {
			if (get_audio_backend()->audio_channel_get_state(s.channel) == AUDIO_PLAYING) {
				fmt::print("channel={} name={} loop={} position={}\n", s.channel.handle, s.name, s.looping, s.position);
			}
		}
		fmt::print("\n");
	}

	if (listener == listener_) {
		return;
	}
	listener = listener_;
	update_volumes();
}

void C1SoundManager::set_listener_world_wrap_width(int32_t world_wrap_width_) {
	if (world_wrap_width == world_wrap_width_) {
		return;
	}
	world_wrap_width = world_wrap_width_;
	update_volumes();
}
