#include "CreatureSkeleton.h"

#include "common/Exception.h"

#include <fmt/core.h>

// Heads have 26 frames but limb data for only 10 views: 0-9 are the views with eyes open,
// 10-12 are expressions facing the camera, 13-22 are 0-9 with eyes closed and 23-25 are
// the expressions with eyes closed.
static size_t limb_data_index(int32_t frame) {
	if (frame >= 0 && frame < 10) {
		return static_cast<size_t>(frame);
	}
	if (frame >= 13 && frame < 23) {
		return static_cast<size_t>(frame - 13);
	}
	if ((frame >= 10 && frame < 13) || (frame >= 23 && frame < 26)) {
		return 8;
	}
	throw Exception(fmt::format("Limb frame {} out of range", frame));
}

static const sfc::LimbData& limb_data(const SkeletonLimb& limb) {
	return (*limb.limb_data)[limb_data_index(limb.frame)];
}

static SkeletonPoint attachment(const SkeletonPose& pose, CreatureLimbChain chain) {
	if (pose.body_frame < 0 || pose.body_frame >= 10) {
		throw Exception(fmt::format("Body frame {} out of range", pose.body_frame));
	}
	const auto& p = (*pose.body_data)[static_cast<size_t>(chain)][static_cast<size_t>(pose.body_frame)];
	return {p.x, p.y};
}

SkeletonPoint skeleton_chain_tip(const SkeletonPose& pose, CreatureLimbChain chain) {
	// each limb's start sits on the previous joint, so the tip is the attachment point
	// plus each limb's (end - start)
	SkeletonPoint tip = attachment(pose, chain);
	for (const auto& limb : pose.chains[static_cast<size_t>(chain)]) {
		const auto& d = limb_data(limb);
		tip.x += d.endx - d.startx;
		tip.y += d.endy - d.starty;
	}
	return tip;
}

SkeletonPositions position_skeleton(const SkeletonPose& pose, bool left_foot_down, int32_t footx, int32_t footy) {
	SkeletonPositions result;

	auto foot = skeleton_chain_tip(pose, left_foot_down ? CHAIN_LEFT_LEG : CHAIN_RIGHT_LEG);
	result.body = {footx - foot.x, footy - foot.y};

	for (size_t c = 0; c < pose.chains.size(); ++c) {
		auto joint = attachment(pose, static_cast<CreatureLimbChain>(c));
		joint.x += result.body.x;
		joint.y += result.body.y;
		for (const auto& limb : pose.chains[c]) {
			const auto& d = limb_data(limb);
			SkeletonPoint position{joint.x - d.startx, joint.y - d.starty};
			result.chains[c].push_back(position);
			joint = {position.x + d.endx, position.y + d.endy};
		}
	}
	return result;
}

int32_t skeleton_frame_for_pose_char(char c, CreatureFacing facing) {
	if (c == 'X') {
		return -1;
	}
	if (c == '4') {
		return 8; // towards the camera
	}
	if (c < '0' || c > '3') {
		throw Exception(fmt::format("Unknown pose character {:?}", c));
	}
	int32_t angle = c - '0';
	switch (facing) {
		case FACING_RIGHT: return angle;
		case FACING_LEFT: return 4 + angle;
		case FACING_CAMERA: return 8; // only one frame each for towards and away
		case FACING_AWAY: return 9;
	}
	return angle;
}

bool skeleton_facing_for_pose_char(char c, CreatureFacing* facing) {
	// '2' faces right, confirmed by saved creatures; the others follow the same order
	// as openc2e's Creatures 2 support
	switch (c) {
		case '0': *facing = FACING_AWAY; return true;
		case '1': *facing = FACING_CAMERA; return true;
		case '2': *facing = FACING_RIGHT; return true;
		case '3': *facing = FACING_LEFT; return true;
		default: return false;
	}
}

int32_t skeleton_z_offset(CreatureLimbChain chain, size_t index_in_chain, CreatureFacing facing) {
	const int32_t depth = static_cast<int32_t>(index_in_chain) + 1;
	// which side of the body is nearer the camera
	const bool left_is_far = facing != FACING_LEFT;
	switch (chain) {
		case CHAIN_HEAD: return 3;
		case CHAIN_LEFT_LEG:
		case CHAIN_LEFT_ARM: return left_is_far ? -depth : depth;
		case CHAIN_RIGHT_LEG:
		case CHAIN_RIGHT_ARM: return left_is_far ? depth : -depth;
		case CHAIN_TAIL: return -4;
	}
	return 0;
}

// Head frames 13-25 are frames 0-12 with the eyes closed.
static int32_t head_frame_with_eyes(int32_t frame, bool eyes_open) {
	int32_t open_frame = frame >= 13 ? frame - 13 : frame;
	return eyes_open ? open_frame : open_frame + 13;
}

void skeleton_apply_pose_string(SkeletonPose& pose, const std::string& p, bool eyes_open, CreatureFacing* facing, CreatureFacing attention_facing) {
	if (p.size() != 15) {
		throw Exception(fmt::format("Creature pose string should be 15 characters, but got {:?}", p));
	}

	CreatureFacing new_facing = *facing;
	if (p[0] == '?') {
		new_facing = attention_facing;
	} else if (p[0] == '!') {
		new_facing = attention_facing == FACING_RIGHT ? FACING_LEFT : attention_facing == FACING_LEFT ? FACING_RIGHT : attention_facing;
	} else if (!skeleton_facing_for_pose_char(p[0], &new_facing) && p[0] != 'X') {
		throw Exception(fmt::format("Unknown direction in creature pose string {:?}", p));
	}

	// pose string order: head, body, left leg (3), right leg (3), left arm (2), right arm (2), tail (2)
	struct Slot {
		int32_t* frame;
		bool is_head;
	};
	std::vector<Slot> slots;
	auto add_chain = [&](CreatureLimbChain chain, size_t count) {
		auto& limbs = pose.chains[static_cast<size_t>(chain)];
		for (size_t i = 0; i < count; ++i) {
			slots.push_back({i < limbs.size() ? &limbs[i].frame : nullptr, chain == CHAIN_HEAD});
		}
	};
	add_chain(CHAIN_HEAD, 1);
	slots.push_back({&pose.body_frame, false});
	add_chain(CHAIN_LEFT_LEG, 3);
	add_chain(CHAIN_RIGHT_LEG, 3);
	add_chain(CHAIN_LEFT_ARM, 2);
	add_chain(CHAIN_RIGHT_ARM, 2);
	add_chain(CHAIN_TAIL, 2);

	for (size_t i = 0; i < slots.size(); ++i) {
		if (!slots[i].frame) {
			continue;
		}
		const char c = p[i + 1];
		int32_t& frame = *slots[i].frame;
		int32_t new_frame;
		if (c == '?' && slots[i].is_head) {
			// look up or down at the object of attention
			// TODO: creatures don't have an object of attention yet, so look straight ahead
			new_frame = skeleton_frame_for_pose_char('1', new_facing);
		} else if (c == 'X') {
			// keep the angle, but turn with the body
			int32_t old = slots[i].is_head ? head_frame_with_eyes(frame, true) : frame;
			new_frame = old < 8 ? skeleton_frame_for_pose_char(static_cast<char>('0' + old % 4), new_facing) : old;
		} else {
			new_frame = skeleton_frame_for_pose_char(c, new_facing);
		}
		frame = slots[i].is_head ? head_frame_with_eyes(new_frame, eyes_open) : new_frame;
	}
	*facing = new_facing;
}

bool skeleton_update_planted_foot(const SkeletonPose& pose, bool* left_foot_down, int32_t* footx) {
	auto planted = skeleton_chain_tip(pose, *left_foot_down ? CHAIN_LEFT_LEG : CHAIN_RIGHT_LEG);
	auto swinging = skeleton_chain_tip(pose, *left_foot_down ? CHAIN_RIGHT_LEG : CHAIN_LEFT_LEG);
	if (swinging.y <= planted.y) {
		return false;
	}
	*footx += swinging.x - planted.x;
	*left_foot_down = !*left_foot_down;
	return true;
}
