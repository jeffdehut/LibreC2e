#pragma once

#include "common/HeapValue.h"
#include <stdint.h>

class EngineContext {
	uint64_t m_generation;

  public:
	// singleton, instantiated at bottom of file
	EngineContext(const struct EngineContextConstructTag&);
	EngineContext(const EngineContext&) = delete;
	EngineContext(EngineContext&&) = delete;
	EngineContext& operator=(const EngineContext&) = delete;
	EngineContext& operator=(EngineContext&&) = delete;
	void reset();
	uint64_t generation() const { return m_generation; }

	// all the parts
	HeapValue<class MessageManager> messages;
	HeapValue<class ImageManager> images;
	HeapValue<class MacroManager> macros;
	HeapValue<class MapManager> map;
	HeapValue<class C1MusicManager> music;
	HeapValue<class ObjectManager> objects;
	HeapValue<class PathManager> paths;
	HeapValue<class PointerManager> pointer;
	HeapValue<class Scriptorium> scriptorium;
	HeapValue<class C1SoundManager> sounds;
	HeapValue<class ViewportManager> viewport;
	HeapValue<struct WorldState> world;
};

extern EngineContext g_engine_context;