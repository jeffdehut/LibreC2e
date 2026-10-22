#pragma once

#include "EngineContext.h"
#include "Object.h"
#include "ObjectHandle.h"
#include "common/Ranges.h"
#include "common/slotmap/DenseSlotMap.h"

#include <memory>
#include <vector>
#include <stdint.h>

class ObjectManager {
  private:
	DenseSlotMap<std::unique_ptr<Object>> m_pool;
	std::vector<ObjectHandle> m_to_destroy;
	uint64_t m_revision = 0;

	auto as_ptr_range() {
		return make_transform_view(m_pool, [](auto&& p) { return p.get(); });
	}

  public:
	ObjectManager() {}

	template <typename T>
	auto add() -> std::enable_if_t<std::is_base_of<Object, T>::value, ObjectHandle> {
		auto t = new T();
		ObjectHandle handle = m_pool.insert(std::unique_ptr<Object>(t));
		t->uid = handle;
		++m_revision;
		return handle;
	}

	template <typename T>
	auto add() -> std::enable_if_t<std::is_pointer<T>::value, ObjectHandle> {
		return add<std::remove_pointer_t<T>>();
	}

	Object* try_get(ObjectHandle handle) {
		auto* obj = m_pool.try_get(handle);
		if (!obj) {
			return nullptr;
		}
		return obj->get();
	}

	auto begin() {
		return as_ptr_range().begin();
	}

	auto end() {
		return as_ptr_range().end();
	}

    uint64_t revision() const { return m_revision; }
    void tick() {
        // A creature can create its sleep bubble during this pass. New objects
        // must not invalidate iteration or receive an allocation-dependent turn.
        std::vector<ObjectHandle> scheduled;
        for (auto* o : *this) scheduled.push_back(o->uid);
        for (auto handle : scheduled) if (auto* o = try_get(handle)) o->tick();
    }

	// KILL can happen in the middle of a script or while iterating over objects, so
	// objects are destroyed at the end of the tick.
	void destroy_later(ObjectHandle handle) {
		m_to_destroy.push_back(handle);
	}

	// Only for unwinding a failed load, before the new object becomes usable.
	void discard_failed_creation(ObjectHandle handle) { m_pool.erase(handle); ++m_revision; }

	void destroy_pending();

	int32_t count_classifier(int32_t family, int32_t genus, int32_t species) {
		int32_t result = 0;
		for (auto* o : *this) {
			if (
				(o->family == family || family == 0) && (o->genus == genus || genus == 0) && (o->species == species || species == 0)) {
				result++;
			}
		}
		return result;
	}
};
