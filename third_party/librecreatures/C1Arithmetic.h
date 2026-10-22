// SPDX-License-Identifier: GPL-3.0-only
// Adapted from LibreCreatures by h4rm0n1c and contributors, commit
// 907b8642f79c2688bd8a1c3f43e401a5f9431c8c, src/c1/scripting/macro.cpp,
// Macro::execute_variable_arithmetic_command. See NOTICE.md and LICENSE.
// openc2e adaptation: standalone signed-value API, optional zero-divisor result,
// and explicit bit-preserving conversion. The Windows host/interpreter is not imported.
#pragma once

#include <bit>
#include <cstdint>
#include <optional>

namespace openc1::librecreatures {
inline std::int32_t signed_bits(std::uint32_t bits) {
	return std::bit_cast<std::int32_t>(bits);
}
inline std::int32_t add(std::int32_t left, std::int32_t right) {
	return signed_bits(static_cast<std::uint32_t>(left) + static_cast<std::uint32_t>(right));
}
inline std::int32_t subtract(std::int32_t left, std::int32_t right) {
	return signed_bits(static_cast<std::uint32_t>(left) - static_cast<std::uint32_t>(right));
}
inline std::int32_t multiply(std::int32_t left, std::int32_t right) {
	return signed_bits(static_cast<std::uint32_t>(left) * static_cast<std::uint32_t>(right));
}
inline std::int32_t negate(std::int32_t value) {
	return signed_bits(0u - static_cast<std::uint32_t>(value));
}
inline std::optional<std::int32_t> divide(std::int32_t left, std::int32_t right) {
	if (!right) return std::nullopt;
	return signed_bits(static_cast<std::uint32_t>(static_cast<std::int64_t>(left) / right));
}
inline std::optional<std::int32_t> remainder(std::int32_t left, std::int32_t right) {
	if (!right) return std::nullopt;
	return signed_bits(static_cast<std::uint32_t>(static_cast<std::int64_t>(left) % right));
}
} // namespace openc1::librecreatures
