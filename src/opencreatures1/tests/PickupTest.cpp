#include "EngineContext.h"
#include "MapManager.h"
#include "MessageManager.h"
#include "PointerManager.h"
#include "Scriptorium.h"
#include "MacroManager.h"
#include "objects/ObjectManager.h"
#include "objects/PointerTool.h"
#include "objects/Vehicle.h"
#include "objects/SimpleObject.h"
#include "objects/Creature.h"
#include "fileformats/sfc/CBiochemistry.h"

#include <gtest/gtest.h>

TEST(PointerMessages, RefusedCreatureInteractionsDoNotCrashOrBlockDelivery) {
	// Eden's hand is a simple object (2 1 1) with touch_bhvr 0. A Norn may still
	// attend to it and send these messages; each should disappoint the Norn.
	for (auto message : {MESSAGE_ACTIVATE1, MESSAGE_ACTIVATE2, MESSAGE_DEACTIVATE}) {
		g_engine_context.reset();
		auto pointer = g_engine_context.objects->add<PointerTool>();
		auto* hand = g_engine_context.objects->try_get(pointer)->as_pointer_tool();
		hand->family = 2;
		hand->genus = hand->species = 1;
		hand->actv = ACTV_ACTIVE1;
		hand->touch_bhvr = 0;
		auto creature = g_engine_context.objects->add<Creature>();
		auto* norn = g_engine_context.objects->try_get(creature)->as_creature();
		norn->dead = norn->asleep = 0;
		norn->biochemistry = std::make_shared<sfc::CBiochemistryV1>();
		norn->stimuli[0].chem0 = 12;
		norn->stimuli[0].amount0 = 7;
		auto ball = g_engine_context.objects->add<SimpleObject>();
		auto* ball_obj = g_engine_context.objects->try_get(ball);
		ball_obj->family = 2;
		ball_obj->genus = 5;
		ball_obj->species = 1;
		g_engine_context.scriptorium->add(2, 5, 1, SCRIPT_ACTIVATE1, "endm");
		g_engine_context.messages->mesg_writ(creature, pointer, message);
		g_engine_context.messages->mesg_writ(pointer, ball, MESSAGE_ACTIVATE1);
		EXPECT_NO_THROW(g_engine_context.messages->tick());
		EXPECT_EQ(hand->actv, ACTV_ACTIVE1);
		EXPECT_EQ(norn->biochemistry->chemicals[12].concentration, 7);
		EXPECT_EQ(ball_obj->actv, ACTV_ACTIVE1);
		EXPECT_TRUE(g_engine_context.messages->m_immediate_message_queue.empty());
	}
	g_engine_context.reset();
}

TEST(PointerMessages, ScriptMessagesUseNormalActivationHandlers) {
	g_engine_context.reset();
	auto pointer = g_engine_context.objects->add<PointerTool>();
	auto* hand = g_engine_context.objects->try_get(pointer)->as_pointer_tool();
	hand->family = 2;
	hand->genus = hand->species = 1;
	g_engine_context.scriptorium->add(2, 1, 1, SCRIPT_ACTIVATE1, "setv obv0 11,endm");
	g_engine_context.scriptorium->add(2, 1, 1, SCRIPT_ACTIVATE2, "setv obv0 22,endm");
	g_engine_context.scriptorium->add(2, 1, 1, SCRIPT_DEACTIVATE, "setv obv0 33,endm");
	auto sender = g_engine_context.objects->add<SimpleObject>();
	int expected = 11;
	for (auto message : {MESSAGE_ACTIVATE1, MESSAGE_ACTIVATE2, MESSAGE_DEACTIVATE}) {
		g_engine_context.messages->mesg_writ(sender, pointer, message);
		EXPECT_NO_THROW(g_engine_context.messages->tick());
		g_engine_context.macros->tick();
		EXPECT_EQ(hand->obv0, expected);
		expected += 11;
	}
	g_engine_context.reset();
}

static ObjectHandle add_simple_object(uint8_t attr) {
	auto handle = g_engine_context.objects->add<SimpleObject>();
	auto* obj = g_engine_context.objects->try_get(handle);
	obj->attr = attr;
	obj->movement_status = MOVEMENT_AUTONOMOUS;
	return handle;
}

TEST(Pickup, HandPicksUpMouseableObjectsOnly) {
	g_engine_context.reset();
	auto pointer = g_engine_context.objects->add<PointerTool>();
	g_engine_context.pointer->m_pointer_tool = pointer;

	auto jar = add_simple_object(ATTR_MOUSEABLE | ATTR_CARRYABLE | ATTR_ACTIVATEABLE | ATTR_WALLBOUND); // honey jars: attr 71
	auto incubator = add_simple_object(ATTR_ACTIVATEABLE); // attr 4

	g_engine_context.messages->mesg_writ(pointer, incubator, MESSAGE_PICKUP);
	g_engine_context.messages->tick();
	EXPECT_FALSE(g_engine_context.objects->try_get(incubator)->carrier);

	g_engine_context.messages->mesg_writ(pointer, jar, MESSAGE_PICKUP);
	g_engine_context.messages->tick();
	auto* jar_obj = g_engine_context.objects->try_get(jar);
	EXPECT_EQ(jar_obj->carrier, pointer);
	EXPECT_EQ(jar_obj->movement_status, MOVEMENT_MOUSEDRIVEN);
	EXPECT_EQ(g_engine_context.pointer->m_carried_object, jar);

	g_engine_context.messages->mesg_writ(pointer, jar, MESSAGE_DROP);
	g_engine_context.messages->tick();
	EXPECT_FALSE(jar_obj->carrier);
	EXPECT_EQ(jar_obj->movement_status, MOVEMENT_AUTONOMOUS);
	EXPECT_FALSE(g_engine_context.pointer->m_carried_object);
	g_engine_context.reset();
}

TEST(Pickup, HeldObjectsAreOnTopAndLandInTheirRoom) {
	// batch 3, Q21 and Q22: the hand's plane is 9999; dropped, an attr 64 object is limited
	// to the room its bottom edge is nearest to
	g_engine_context.reset();
	g_engine_context.map->add_room(Room{0, 0, 1000, 400, 1});
	g_engine_context.map->add_room(Room{0, 400, 1000, 800, 0});
	auto pointer = g_engine_context.objects->add<PointerTool>();
	g_engine_context.pointer->m_pointer_tool = pointer;
	auto ball = add_simple_object(ATTR_MOUSEABLE | ATTR_WALLBOUND);
	auto* obj = static_cast<SimpleObject*>(g_engine_context.objects->try_get(ball));
	obj->z_order = 500;
	obj->part = DullPart(ImageGallery{}, 0, 100, 700, 500);
	obj->limit = Rect2i{0, 0, 1000, 400};

	g_engine_context.messages->mesg_writ(pointer, ball, MESSAGE_PICKUP);
	g_engine_context.messages->tick();
	EXPECT_EQ(obj->part.z_order(), 9998); // just behind the hand (9999), which holds it

	g_engine_context.messages->mesg_writ(pointer, ball, MESSAGE_DROP);
	g_engine_context.messages->tick();
	EXPECT_EQ(obj->part.z_order(), 500);
	EXPECT_EQ(obj->limit, (Rect2i{0, 400, 1000, 400}));
	// and it lands at once, its bottom edge on the room's floor (batch 4, Q1)
	EXPECT_EQ(obj->get_bbox().bottom(), 800);
	EXPECT_EQ(obj->get_bbox().x, 100);
	g_engine_context.reset();
}

TEST(Pickup, FloatingObjectsStayWhereTheyAreLetGo) {
	// batch 4, Q1 and Q3: attr 32 objects aren't put on a floor
	g_engine_context.reset();
	g_engine_context.map->add_room(Room{0, 0, 1000, 800, 1});
	auto pointer = g_engine_context.objects->add<PointerTool>();
	g_engine_context.pointer->m_pointer_tool = pointer;
	auto balloon = add_simple_object(ATTR_MOUSEABLE | ATTR_FLOATABLE | ATTR_WALLBOUND);
	auto* obj = static_cast<SimpleObject*>(g_engine_context.objects->try_get(balloon));
	obj->z_order = 500;
	obj->part = DullPart(ImageGallery{}, 0, 300, 200, 500);

	g_engine_context.messages->mesg_writ(pointer, balloon, MESSAGE_PICKUP);
	g_engine_context.messages->tick();
	g_engine_context.messages->mesg_writ(pointer, balloon, MESSAGE_DROP);
	g_engine_context.messages->tick();
	EXPECT_EQ(obj->get_bbox().y, 200);
	EXPECT_EQ(obj->movement_status, MOVEMENT_FLOATING);
	g_engine_context.reset();
}

TEST(Pickup, DropScriptPoseIsUsedForTheFloor) {
	// batch 6, Q1b: the drop script's first part runs before the object is seated, so the
	// carrot's "pose 6" (lying down, shorter) is the picture that lands on the floor
	g_engine_context.reset();
	g_engine_context.map->add_room(Room{0, 0, 1000, 800, 0});
	auto pointer = g_engine_context.objects->add<PointerTool>();
	g_engine_context.pointer->m_pointer_tool = pointer;
	g_engine_context.scriptorium->add(2, 6, 3, SCRIPT_DROP, "pose 1,endm");
	auto carrot = add_simple_object(ATTR_MOUSEABLE | ATTR_WALLBOUND);
	auto* obj = static_cast<SimpleObject*>(g_engine_context.objects->try_get(carrot));
	obj->family = 2;
	obj->genus = 6;
	obj->species = 3;
	ImageGallery pictures;
	pictures.texture_locations = {Rect2i{0, 0, 16, 49}, Rect2i{0, 0, 52, 15}};
	obj->part = DullPart(pictures, 0, 300, 200, 500);

	g_engine_context.messages->mesg_writ(pointer, carrot, MESSAGE_PICKUP);
	g_engine_context.messages->tick();
	g_engine_context.messages->mesg_writ(pointer, carrot, MESSAGE_DROP);
	g_engine_context.messages->tick();
	EXPECT_EQ(obj->part.pose(), 1);
	EXPECT_EQ(obj->get_bbox().bottom(), 800);
	EXPECT_EQ(obj->get_bbox().x, 300);
	g_engine_context.reset();
}

TEST(Pickup, EggDropMessageActivatesAndAnimatesTheIncubator) {
	g_engine_context.reset();
	auto pointer = g_engine_context.objects->add<PointerTool>();
	g_engine_context.pointer->m_pointer_tool = pointer;
	auto inc_handle = g_engine_context.objects->add<Vehicle>();
	auto* inc = g_engine_context.objects->try_get(inc_handle)->as_vehicle();
	inc->family = 3;
	inc->genus = 1;
	inc->species = 3;
	inc->attr = ATTR_CONTAINER;
	inc->actv = ACTV_INACTIVE;
	inc->cabin = Rect2i{50, 65, 40, 60};
	ImageGallery pictures;
	pictures.texture_locations.resize(4, Rect2i{0, 0, 140, 180});
	inc->parts.resize(3);
	for (auto& part : inc->parts) static_cast<DullPart&>(part) = DullPart(pictures, 0, 100, 100, 100);
	auto egg_handle = add_simple_object(ATTR_MOUSEABLE | ATTR_WALLBOUND);
	auto* egg = g_engine_context.objects->try_get(egg_handle)->as_simple_object();
	egg->family = 2;
	egg->genus = 5;
	egg->species = 2;
	egg->carrier = pointer;
	ImageGallery eggs;
	eggs.texture_locations = {Rect2i{0, 0, 10, 10}};
	egg->part = DullPart(eggs, 0, 160, 175, 2000);
	// These are the relevant commands from Eden's egg-drop and incubator scripts.
	g_engine_context.scriptorium->add(2, 5, 2, SCRIPT_DROP, "mesg writ carr 0,endm");
	g_engine_context.scriptorium->add(3, 1, 3, SCRIPT_ACTIVATE1, "part 2,anim [12],endm");
	g_engine_context.messages->mesg_writ(pointer, egg_handle, MESSAGE_DROP);
	g_engine_context.messages->tick();
	EXPECT_EQ(egg->carrier, inc_handle);
	EXPECT_EQ(inc->actv, ACTV_ACTIVE1);
	g_engine_context.macros->tick();
	EXPECT_TRUE(inc->parts[2].has_animation());
	EXPECT_EQ(inc->parts[2].pose(), 1);
	g_engine_context.reset();
}

TEST(Pickup, ReleaseHighInLowerRoomUsesItsSupportingFloor) {
	for (float x : {0.f, 50.f, 80.f}) {
		g_engine_context.reset();
		g_engine_context.map->add_room(Room{0, 0, 100, 100, 1});
		g_engine_context.map->add_room(Room{0, 100, 100, 300, 1});
		auto id = g_engine_context.objects->add<SimpleObject>();
		auto* object = g_engine_context.objects->try_get(id)->as_simple_object();
		ImageGallery art; art.texture_locations.push_back(Rect2i{0, 0, 20, 20});
		object->part = DullPart(art, 0, x, 105, 50);
		object->attr = ATTR_WALLBOUND;
		object->land_object(nullptr);
		EXPECT_EQ(object->limit, (Rect2i{0, 100, 100, 200}));
		EXPECT_EQ(object->get_bbox().bottom(), 300);
		EXPECT_EQ(object->get_bbox().x, x);
	}
	g_engine_context.reset();
}

TEST(Pickup, CreatureReleaseUsesItsFloorWhenHeldItemExtendsPastLedge) {
	g_engine_context.reset();
	g_engine_context.map->add_room(Room{0, 0, 100, 100, 1});
	g_engine_context.map->add_room(Room{0, 100, 200, 300, 1});
	auto holder = g_engine_context.objects->add<Creature>();
	auto* creature = g_engine_context.objects->try_get(holder)->as_creature();
	creature->footx = 95; creature->footy = 100;
	auto id = g_engine_context.objects->add<SimpleObject>();
	auto* object = g_engine_context.objects->try_get(id)->as_simple_object();
	ImageGallery art; art.texture_locations.push_back(Rect2i{0, 0, 20, 20});
	object->part = DullPart(art, 0, 105, 105, 50);
	object->attr = ATTR_WALLBOUND;
	object->land_object(creature);
	EXPECT_EQ(object->limit, (Rect2i{0, 0, 100, 100}));
	EXPECT_EQ(object->get_bbox().bottom(), 100);
	EXPECT_EQ(object->get_bbox().x, 80);
	g_engine_context.reset();
}

TEST(Pickup, WhatTheHandHeldWhenSavedIsHeldAgainAfterLoading) {
	// As LibreCreatures has it: only the object's side of the hold is saved, so it hung in
	// mid-air and the hand couldn't take it again. Only one thing fits in the hand.
	g_engine_context.reset();
	auto pointer = g_engine_context.objects->add<PointerTool>();
	g_engine_context.pointer->m_pointer_tool = pointer;
	auto jar = add_simple_object(ATTR_MOUSEABLE | ATTR_CARRYABLE);
	auto spare = add_simple_object(ATTR_MOUSEABLE | ATTR_CARRYABLE);
	for (auto handle : {jar, spare}) {
		auto* obj = g_engine_context.objects->try_get(handle);
		obj->carrier = pointer;
		obj->movement_status = MOVEMENT_MOUSEDRIVEN;
	}
	g_engine_context.pointer->restore_hold();
	EXPECT_EQ(g_engine_context.pointer->m_carried_object, jar);
	EXPECT_TRUE(g_engine_context.objects->try_get(pointer)->attr & ATTR_INVISIBLE);
	EXPECT_EQ(g_engine_context.objects->try_get(jar)->get_z_order(), HAND_PLANE - 1);
	EXPECT_FALSE(g_engine_context.objects->try_get(spare)->carrier);
	EXPECT_EQ(g_engine_context.objects->try_get(spare)->movement_status, MOVEMENT_AUTONOMOUS);
}

TEST(Pickup, ObjectTimersCountDownAsTheOriginalSavesThem) {
	// batch 6: `tick n` sets the period and the countdown, which goes down by 1 a tick, and at 0
	// the timer fires and starts again from the period. Without a timer script it keeps going.
	g_engine_context.reset();
	auto handle = add_simple_object(ATTR_MOUSEABLE);
	auto* obj = g_engine_context.objects->try_get(handle);
	obj->tick_value = obj->timer_countdown = 3;
	obj->tick();
	EXPECT_EQ(obj->timer_countdown, 2);
	obj->tick();
	EXPECT_EQ(obj->timer_countdown, 1);
	obj->tick(); // fires
	EXPECT_EQ(obj->timer_countdown, 3);
	EXPECT_EQ(obj->tick_value, 3);
}
