#include "EngineContext.h"
#include "CreatureBrain.h"
#include "fileformats/sfc/CBrain.h"
#include "MacroManager.h"
#include "Macro.h"
#include "MapManager.h"
#include "MessageManager.h"
#include "Scriptorium.h"
#include "WorldState.h"
#include "objects/Creature.h"
#include "objects/Lift.h"
#include "objects/ObjectManager.h"
#include "objects/PointerTool.h"
#include "objects/SimpleObject.h"

#include <gtest/gtest.h>

class LiftRide : public testing::Test {
protected:
	Lift* lift = nullptr;
	Creature* passenger = nullptr;
	void SetUp() override {
		g_engine_context.reset();
		for (int floor : {250, 400, 550}) {
			g_engine_context.map->add_room(Room{0, floor - 150, 1000, floor, 0});
		}
		auto lift_handle = g_engine_context.objects->add<Lift>();
		lift = g_engine_context.objects->try_get(lift_handle)->as_lift();
		lift->family = 3;
		lift->genus = 2;
		lift->species = 1;
		lift->actv = ACTV_INACTIVE;
		lift->attr = ATTR_CONTAINER;
		lift->cabin = Rect2i{7, 34, 77, 116};
		lift->next_or_current_floor = 0;
		for (int floor : {250, 400, 550}) lift->floors.push_back(floor);
		ImageGallery pictures;
		pictures.texture_locations.resize(4, Rect2i{0, 0, 100, 160});
		lift->parts.resize(3);
		for (auto& part : lift->parts) static_cast<DullPart&>(part) = DullPart(pictures, 0, 100, 100, 200);
		auto creature_handle = g_engine_context.objects->add<Creature>();
		passenger = g_engine_context.objects->try_get(creature_handle)->as_creature();
		passenger->family = 4;
		passenger->genus = passenger->species = 1;
		passenger->attr = ATTR_WALLBOUND;
		passenger->movement_status = MOVEMENT_AUTONOMOUS;
		passenger->body = std::make_unique<Body>();
		ImageGallery body;
		body.texture_locations.resize(10, Rect2i{0, 0, 20, 40});
		static_cast<DullPart&>(*passenger->body) = DullPart(body, 0, 130, 210, 100);
		for (auto& point : passenger->body->body_data[CHAIN_LEFT_LEG]) point = {0, 40};
		for (auto& point : passenger->body->body_data[CHAIN_RIGHT_LEG]) point = {0, 40};
		passenger->footx = 130;
		passenger->footy = 250;
		passenger->update_limits();
		// The shipped lift's commands, omitting sound and stimulus commands.
		// a lift of our own: up, down and stop (not the shipped world's scripts)
		g_engine_context.scriptorium->add(3, 2, 1, SCRIPT_ACTIVATE1,
			"doif obv0 eq 0,gpas,setv obv0 1,setv yvec 768,setv xvec 0,part 2,pose 1,endi,endm");
		g_engine_context.scriptorium->add(3, 2, 1, SCRIPT_ACTIVATE2,
			"doif obv0 eq 0,gpas,setv obv0 1,setv yvec -768,setv xvec 0,part 2,pose 2,endi,endm");
		g_engine_context.scriptorium->add(3, 2, 1, SCRIPT_DEACTIVATE,
			"setv obv0 1,setv actv 0,part 2,pose 0,dpas,tick 30,endm");
		g_engine_context.scriptorium->add(3, 2, 1, SCRIPT_TIMER, "setv obv0 0,tick 0,endm");
	}
	void TearDown() override { g_engine_context.reset(); }
	void tick(int ticks) {
		for (int i = 0; i < ticks; ++i) {
			g_engine_context.world->tick++;
			lift->tick();
			g_engine_context.macros->tick();
			if (g_engine_context.world->tick % 4 == 0) g_engine_context.messages->tick();
		}
	}
	SimpleObject* drop_cargo(uint8_t attr = ATTR_MOUSEABLE | ATTR_CARRYABLE | ATTR_WALLBOUND) {
		auto hand = g_engine_context.objects->add<PointerTool>();
		g_engine_context.pointer->m_pointer_tool = hand;
		auto handle = g_engine_context.objects->add<SimpleObject>();
		auto* cargo = g_engine_context.objects->try_get(handle)->as_simple_object();
		cargo->attr = attr;
		cargo->z_order = 500;
		ImageGallery pictures;
		pictures.texture_locations = {Rect2i{0, 0, 20, 20}};
		cargo->part = DullPart(pictures, 0, 130, 210, HAND_PLANE);
		cargo->carrier = hand;
		cargo->movement_status = MOVEMENT_MOUSEDRIVEN;
		g_engine_context.pointer->m_carried_object = handle;
		g_engine_context.messages->mesg_writ(hand, handle, MESSAGE_DROP);
		g_engine_context.messages->tick();
		return cargo;
	}
};

TEST_F(LiftRide, HandCanRetrieveObjectDroppedInsideLift) {
	auto* cargo = drop_cargo();
	ASSERT_EQ(cargo->carrier, lift->uid);
	ASSERT_EQ(cargo->movement_status, MOVEMENT_INVEHICLE);
	ASSERT_EQ(cargo->get_bbox().bottom(), 250);
	auto hand = g_engine_context.pointer->m_pointer_tool;
	// Take it out both while parked and after a ride, as with a previously saved passenger.
	for (bool ride : {false, true}) {
		if (ride) {
			lift->handle_mesg_activate1({hand, lift->uid, MESSAGE_ACTIVATE1});
			tick(100);
			ASSERT_EQ(cargo->carrier, lift->uid); // DPAS only releases creatures.
			ASSERT_EQ(cargo->get_bbox().bottom(), 400);
		}
		g_engine_context.messages->mesg_writ(hand, cargo->uid, MESSAGE_PICKUP);
		g_engine_context.messages->tick();
		ASSERT_EQ(cargo->carrier, hand);
		EXPECT_EQ(cargo->movement_status, MOVEMENT_MOUSEDRIVEN);
		EXPECT_EQ(g_engine_context.pointer->m_carried_object, cargo->uid);
		EXPECT_EQ(cargo->part.z_order(), HAND_PLANE - 1); // just behind the hand, which holds it
		EXPECT_EQ(cargo->limit, (Rect2i{-100000, -100000, 200000, 200000}));
		auto position = cargo->get_bbox();
		lift->add_position(0, -10);
		EXPECT_EQ(cargo->get_bbox(), position); // no longer follows the old carrier
		lift->add_position(0, 10);
		if (!ride) {
			g_engine_context.messages->mesg_writ(hand, cargo->uid, MESSAGE_DROP);
			g_engine_context.messages->tick();
			ASSERT_EQ(cargo->carrier, lift->uid);
		}
	}
	// Releasing outside the cabin restores the object's normal plane and room limits.
	cargo->set_position(300, 350);
	g_engine_context.messages->mesg_writ(hand, cargo->uid, MESSAGE_DROP);
	g_engine_context.messages->tick();
	EXPECT_FALSE(cargo->carrier);
	EXPECT_FALSE(g_engine_context.pointer->m_carried_object);
	EXPECT_EQ(cargo->movement_status, MOVEMENT_AUTONOMOUS);
	EXPECT_EQ(cargo->part.z_order(), 500);
	EXPECT_EQ(cargo->get_bbox().bottom(), 400);
}

TEST_F(LiftRide, CreatureCanRetrieveCarryableObjectInsideLift) {
	auto* cargo = drop_cargo();
	ASSERT_EQ(cargo->carrier, lift->uid);
	g_engine_context.messages->mesg_writ(passenger->uid, cargo->uid, MESSAGE_PICKUP);
	g_engine_context.messages->tick();
	EXPECT_EQ(cargo->carrier, passenger->uid);
	EXPECT_EQ(cargo->movement_status, MOVEMENT_CARRIED);
}

TEST_F(LiftRide, CabinDoesNotOverrideObjectPickupPermissions) {
	auto* cargo = drop_cargo(ATTR_WALLBOUND);
	ASSERT_EQ(cargo->carrier, lift->uid);
	g_engine_context.messages->mesg_writ(g_engine_context.pointer->m_pointer_tool, cargo->uid, MESSAGE_PICKUP);
	g_engine_context.messages->mesg_writ(passenger->uid, cargo->uid, MESSAGE_PICKUP);
	g_engine_context.messages->tick();
	EXPECT_EQ(cargo->carrier, lift->uid);
	EXPECT_EQ(cargo->movement_status, MOVEMENT_INVEHICLE);
	EXPECT_FALSE(g_engine_context.pointer->m_carried_object);
}

TEST_F(LiftRide, ArrivalReleasesPassengerOntoRoomFloor) {
	lift->handle_mesg_activate1({passenger->uid, lift->uid, MESSAGE_ACTIVATE1});
	tick(3);
	ASSERT_EQ(passenger->carrier, lift->uid);
	tick(100);
	EXPECT_EQ(lift->actv, ACTV_INACTIVE);
	EXPECT_EQ(lift->get_bbox().y + lift->cabin.bottom(), 400);
	EXPECT_FALSE(passenger->carrier);
	EXPECT_EQ(passenger->movement_status, MOVEMENT_AUTONOMOUS);
	EXPECT_EQ(passenger->footy, 400);
	EXPECT_EQ(passenger->limit, (Rect2i{0, 250, 1000, 150}));
	// After release the Norn can move beyond the cabin's horizontal limits.
	passenger->move_ground_to(300, 400);
	passenger->settle_on_limits();
	EXPECT_EQ(passenger->footx, 300);
}

TEST_F(LiftRide, PassengerCanResumeDecisionsAfterCarriedActionStops) {
	lift->handle_mesg_activate1({passenger->uid, lift->uid, MESSAGE_ACTIVATE1});
	tick(3);
	ASSERT_EQ(passenger->carrier, lift->uid);
	// Batch 7 Q4: the egg-laying reflex stops without DONE when carried. Its mark
	// blocks further reflexes until a voluntary decision, not all future decisions.
	g_engine_context.scriptorium->add(4, 0, 0, SCRIPT_INVOLUNTARY1,
		"doif carr eq 0,done,endi,stop,endm");
	passenger->running_involuntary = 1;
	g_engine_context.macros->queue_script(passenger, passenger, SCRIPT_INVOLUNTARY1);
	tick(100);
	ASSERT_FALSE(passenger->carrier);
	ASSERT_EQ(passenger->running_involuntary, 1);
	passenger->brain = std::make_shared<sfc::CBrainV1>();
	passenger->brain->lobes.resize(8);
	for (auto& lobe : passenger->brain->lobes) lobe.num_neurons = 40;
	passenger->brain->neurons.resize(320);
	passenger->brain->lobes[LOBE_DECISION].svrule = {9}; // preserve decision states
	brain_set_input(*passenger->brain, LOBE_DECISION, 11, 200); // go right
	g_engine_context.scriptorium->add(4, 0, 0, SCRIPT_INTRO_EAST, "wait 10,endm");
	passenger->brain_step();
	EXPECT_EQ(passenger->running_involuntary, -1);
	EXPECT_EQ(passenger->current_decision, 11);
}

TEST_F(LiftRide, ReturnTripAlsoReleasesPassenger) {
	lift->handle_mesg_activate1({passenger->uid, lift->uid, MESSAGE_ACTIVATE1});
	tick(100);
	ASSERT_FALSE(passenger->carrier);
	passenger->move_ground_to(130, 400);
	lift->handle_mesg_activate2({passenger->uid, lift->uid, MESSAGE_ACTIVATE2});
	tick(3);
	ASSERT_EQ(passenger->carrier, lift->uid);
	tick(100);
	EXPECT_EQ(lift->next_or_current_floor, 0);
	EXPECT_FALSE(passenger->carrier);
	EXPECT_EQ(passenger->movement_status, MOVEMENT_AUTONOMOUS);
	EXPECT_EQ(passenger->footy, 250);
	EXPECT_EQ(passenger->limit, (Rect2i{0, 100, 1000, 150}));
}

TEST_F(LiftRide, ScriptedMoveOutsideCabinDetachesCargo) {
    auto* cargo = drop_cargo();
    Macro m("mvto 500 350,endm"); m.inst = true; m.ownr = m.targ = cargo->uid;
    g_engine_context.macros->run_now(m, false);
    EXPECT_FALSE(cargo->carrier);
    const auto placed = cargo->get_bbox();
    lift->add_position(0, 20);
    EXPECT_EQ(cargo->get_bbox(), placed);
}
TEST_F(LiftRide, SameFloorGrabAndReleaseDoesNotRetainPassenger) {
    for (int i = 0; i < 20; ++i) {
        lift->vehicle_grab_passengers();
        ASSERT_EQ(passenger->carrier, lift->uid);
        lift->vehicle_drop_passengers();
        EXPECT_FALSE(passenger->carrier);
        EXPECT_EQ(passenger->movement_status, MOVEMENT_AUTONOMOUS);
    }
}
TEST_F(LiftRide, FractionalFloorCrossingStopsAtTheFloor) {
    // as LibreCreatures has it: it stops once its cabin is within its speed's whole pixels of the
    // floor, or past it, where it is, on a whole pixel
    lift->set_position(100, 96.75f); // cabin bottom 246.75
    lift->next_or_current_floor = 0;
    lift->actv = ACTV_ACTIVE1; lift->obv0 = 1; lift->yvel = 2;
    for (int i = 0; i < 10 && lift->yvel != 0; ++i) tick(1);
    EXPECT_EQ(lift->yvel, 0);
    const float cabin = lift->get_bbox().y + lift->cabin.bottom();
    EXPECT_LT(std::abs(cabin - 250), 2.f);
    EXPECT_FLOAT_EQ(lift->get_bbox().y, std::trunc(lift->get_bbox().y));
}

#include "objects/CallButton.h"
TEST_F(LiftRide, EveryOneOfEightAcceptedCallsIsServicedAndExcessIsRejected) {
    lift->floors.clear();
    for (int n = 0; n < 8; ++n) lift->floors.push_back(250 + n * 50);
    lift->next_or_current_floor = -1;
    std::vector<ObjectHandle> buttons;
    for (int n = 0; n < 9; ++n) {
        const auto id = g_engine_context.objects->add<CallButton>();
        auto* button = g_engine_context.objects->try_get(id)->as_call_button();
        button->lift = lift->uid; button->floor = static_cast<uint8_t>(n % 8); button->actv = ACTV_INACTIVE;
        button->handle_mesg_activate1({{}, id, MESSAGE_ACTIVATE1});
        buttons.push_back(id);
    }
    EXPECT_EQ(lift->activated_call_buttons.size(), 8u);
    EXPECT_EQ(g_engine_context.objects->try_get(buttons.back())->actv, ACTV_INACTIVE);
    tick(1600);
    EXPECT_EQ(lift->activated_call_buttons.size(), 0u);
    for (auto id : buttons) EXPECT_EQ(g_engine_context.objects->try_get(id)->actv, ACTV_INACTIVE);
}

TEST_F(LiftRide, CallsAreAnsweredByNearestFloorEarliestSlotFirstAndOnlyThatButtonClears) {
    // as LibreCreatures has it: calls at floors 0 and 2 are equally near the lift at floor 1, so
    // the earlier slot (floor 0) is answered, and arriving clears only its button
    lift->next_or_current_floor = 1;
    lift->set_position(100, 400 - lift->cabin.bottom());
    std::vector<ObjectHandle> buttons;
    for (uint8_t floor : {0, 2}) {
        const auto id = g_engine_context.objects->add<CallButton>();
        auto* button = g_engine_context.objects->try_get(id)->as_call_button();
        button->lift = lift->uid; button->floor = floor; button->actv = ACTV_INACTIVE;
        button->handle_mesg_activate1({{}, id, MESSAGE_ACTIVATE1});
        buttons.push_back(id);
    }
    ASSERT_EQ(lift->activated_call_buttons.size(), 2u);
    for (int i = 0; i < 16 && lift->actv == ACTV_INACTIVE; ++i) tick(1);
    EXPECT_EQ(lift->next_or_current_floor, 0);
    EXPECT_EQ(lift->actv, ACTV_ACTIVE2); // to an earlier floor
    for (int i = 0; i < 200 && lift->actv != ACTV_INACTIVE; ++i) tick(1);
    EXPECT_EQ(lift->actv, ACTV_INACTIVE);
    EXPECT_EQ(g_engine_context.objects->try_get(buttons[0])->actv, ACTV_INACTIVE);
    EXPECT_EQ(g_engine_context.objects->try_get(buttons[1])->actv, ACTV_ACTIVE1);
    EXPECT_EQ(lift->activated_call_buttons.size(), 1u);
}

TEST_F(LiftRide, PassengersAreDrawnBetweenTheCarAndItsFrontAndGetTheirPlaneBack) {
    // as LibreCreatures has it: 5 planes off the vehicle's main part, behind it when that part
    // is at or in front of its second part, in front otherwise
    lift->parts[0].set_z_order(1000);
    lift->parts[1].set_z_order(1100);
    EXPECT_EQ(lift->passenger_plane(), 1005);
    lift->parts[1].set_z_order(990);
    EXPECT_EQ(lift->passenger_plane(), 995);
    passenger->saved_z_order = passenger->z_order = 2000;
    passenger->carrier = lift->uid;
    passenger->movement_status = MOVEMENT_INVEHICLE;
    passenger->keep_passenger_plane();
    EXPECT_EQ(passenger->z_order, 995u);
    passenger->carrier = {};
    passenger->movement_status = MOVEMENT_AUTONOMOUS;
    passenger->keep_passenger_plane();
    EXPECT_EQ(passenger->z_order, 2000u);
}
