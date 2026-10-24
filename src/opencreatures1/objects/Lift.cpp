#include "Lift.h"

#include "CallButton.h"
#include "ObjectManager.h"
#include "SFCSerialization.h"
#include "common/Ranges.h"
#include "fileformats/sfc/CallButton.h"
#include "fileformats/sfc/Lift.h"

void Lift::serialize(SFCContext& ctx, sfc::LiftV1* lift) {
	if (ctx.is_storing()) {
		lift->num_floors = numeric_cast<int32_t>(floors.size());
		lift->next_or_current_floor = next_or_current_floor;

		lift->current_call_button = answering_call;
		lift->delay_ticks_divided_by_32 = call_checks_to_skip;
		for (size_t i = 0; i < floors.size(); ++i) {
			lift->floors[i] = floors[i];
		}
		for (size_t i = 0; i < activated_call_buttons.slot.size(); ++i) {
			lift->activated_call_buttons[i] = dynamic_cast<sfc::CallButtonV1*>(
				ctx.dump_object(g_engine_context.objects->try_get(activated_call_buttons.slot[i])).get());
		}
	} else {
        if (lift->num_floors < 0 || lift->num_floors > 8) throw Exception("Lift floor count exceeds supported capacity (8)");
		next_or_current_floor = lift->next_or_current_floor;
		answering_call = lift->current_call_button >= 0 && lift->current_call_button < 8 ? lift->current_call_button : -1;
		call_checks_to_skip = lift->delay_ticks_divided_by_32;
		for (size_t i = 0; i < numeric_cast<size_t>(lift->num_floors); ++i) {
			floors.push_back(lift->floors[i]);
		}
		// each call in its own slot, as saved
		for (size_t i = 0; i < activated_call_buttons.slot.size(); ++i) {
			activated_call_buttons.slot[i] = ctx.load_object(lift->activated_call_buttons[i]);
		}
	}
	Vehicle::serialize(ctx, lift);
}