#pragma once

#include "fileformats/sfc/Body.h"
#include "fileformats/sfc/Limb.h"

#include <array>
#include <stdint.h>
#include <string>
#include <vector>

// Positions a Creatures 1 creature's body parts. Kept free of engine state so it can be
// checked against the part positions saved in world files.
//
// The body has six attachment points per frame (body data, from Body Data/*.att):
// head, left leg, right leg, left arm, right arm, tail. Each limb has a start point
// (where it joins its parent) and an end point (where its child joins) per frame.

enum CreatureLimbChain {
	CHAIN_HEAD = 0,
	CHAIN_LEFT_LEG = 1,
	CHAIN_RIGHT_LEG = 2,
	CHAIN_LEFT_ARM = 3,
	CHAIN_RIGHT_ARM = 4,
	CHAIN_TAIL = 5,
};

struct SkeletonLimb {
	const std::array<sfc::LimbData, 10>* limb_data;
	int32_t frame; // 0-9, relative to the part's first sprite
};

struct SkeletonPose {
	const std::array<std::array<sfc::Vector2i8, 10>, 6>* body_data;
	int32_t body_frame;
	// each chain lists its limbs from the body outwards, e.g. thigh, shin, foot
	std::array<std::vector<SkeletonLimb>, 6> chains;
};

struct SkeletonPoint {
	int32_t x = 0;
	int32_t y = 0;
};

struct SkeletonPositions {
	SkeletonPoint body;
	std::array<std::vector<SkeletonPoint>, 6> chains;
};

// The tip of a chain (e.g. the bottom of a foot), relative to the body's position.
SkeletonPoint skeleton_chain_tip(const SkeletonPose& pose, CreatureLimbChain chain);

// Positions every part so that the tip of the given leg is at (footx, footy).
SkeletonPositions position_skeleton(const SkeletonPose& pose, bool left_foot_down, int32_t footx, int32_t footy);

// The sprite frame (relative to the part's first sprite) for one character of a pose
// string, given the creature's facing. Frames 0-3 face right, 4-7 face left, 8 faces the
// camera and 9 faces away. Returns -1 for 'X' (keep the current frame).
enum CreatureFacing {
	FACING_RIGHT = 0,
	FACING_LEFT = 1,
	FACING_CAMERA = 2,
	FACING_AWAY = 3,
};
int32_t skeleton_frame_for_pose_char(char c, CreatureFacing facing);

// Decodes the direction character at the start of a pose string. Returns false for
// characters that keep the current facing ('X') or depend on the object of attention.
bool skeleton_facing_for_pose_char(char c, CreatureFacing* facing);

// Z-order offset of each part relative to the body, by facing. Matches saved worlds when
// facing right: the left limbs are behind the body and the right limbs and head in front.
int32_t skeleton_z_offset(CreatureLimbChain chain, size_t index_in_chain, CreatureFacing facing);

// Applies a pose string (see Creature::set_pose_string) to the frames in `pose`. `facing`
// is the current facing and is updated if the pose turns the creature. Head frames include
// the eyes-closed offset. Parts missing from the creature (e.g. a C1 creature's tail) are
// skipped.
// `attention_facing` is used for '?' (towards the object of attention) and '!' (away).
void skeleton_apply_pose_string(SkeletonPose& pose, const std::string& pose_string, bool eyes_open, CreatureFacing* facing, CreatureFacing attention_facing);

// Walking: once the swinging foot comes down below the planted one, it takes over as the
// planted foot. Moves footx along by the distance between the feet. Returns true if the
// planted foot changed.
bool skeleton_update_planted_foot(const SkeletonPose& pose, bool* left_foot_down, int32_t* footx);
