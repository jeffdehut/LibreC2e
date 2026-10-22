#include "MessageManager.h"

#include "EngineContext.h"
#include "MacroManager.h"
#include "common/Exception.h"
#include "fmt/format.h"
#include "objects/Object.h"
#include "objects/ObjectHandle.h"
#include "objects/ObjectManager.h"

void MessageManager::deliver(Object& target, const Message& m) {
	auto* to = &target;
	switch (m.number) {
		case MESSAGE_ACTIVATE1: to->handle_mesg_activate1(m); break;
		case MESSAGE_ACTIVATE2: to->handle_mesg_activate2(m); break;
		case MESSAGE_DEACTIVATE: to->handle_mesg_deactivate(m); break;
		case MESSAGE_HIT: to->handle_mesg_hit(m); break;
		case MESSAGE_PICKUP: to->handle_mesg_pickup(m); break;
		case MESSAGE_DROP: to->handle_mesg_drop(m); break;
		default:
		// Other messages run the event script with the same number, except that
		// message 8 runs the initialise script (7), e.g. the coconut tree sends it
		// to each coconut it creates.
		g_engine_context.macros->queue_script(m.from, m.to, ScriptNumber(m.number == 8 ? SCRIPT_INITIALIZE : m.number));
		break;
	}
}

void MessageManager::hand_message(ObjectHandle from_id, ObjectHandle to_id, MessageNumber message) {
	// Not in the original: the player's own clicks act at once instead of waiting for the next
	// 4-tick delivery (up to about a third of a second at normal speed). Scripts' messages
	// still use the queue.
	auto* to = g_engine_context.objects->try_get(to_id);
	if (!to) {
		return;
	}
	deliver(*to, Message{from_id, to_id, message});
}

void MessageManager::tick() {
	// Drop handlers run the first part of a drop script immediately. Such a script can
	// enqueue another message (the egg activates its incubator this way). Copy each entry
	// and include appended messages: vector growth must neither invalidate our message nor
	// lose the newly queued messages when the queue is cleared below.
	for (size_t i = 0; i < m_immediate_message_queue.size(); ++i) {
		const Message m = m_immediate_message_queue[i];
		auto* to = g_engine_context.objects->try_get(m.to);
		if (!to) {
			fmt::print("WARNING: discarding message with bad `to` object id\n");
			continue;
		}

		deliver(*to, m);
	}
	m_immediate_message_queue.clear();
}

void MessageManager::mesg_writ(ObjectHandle from_id, ObjectHandle to_id, MessageNumber message) {
	// TODO: implement delayed messages

	Message m;
	m.from = from_id;
	m.to = to_id;
	m.number = message;

	m_immediate_message_queue.push_back(m);
}
