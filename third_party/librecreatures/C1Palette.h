// Source-informed adaptation from h4rm0n1c/LibreCreatures, GNU GPL version 3.
// Pin 907b8642f79c2688bd8a1c3f43e401a5f9431c8c: src/c1/display/palette.cpp.
// Adapter accepts the engine's expanded 256-colour palette, preserving reserved
// indices 0..9 and 246..255. See NOTICE.md and LICENSE.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace openc1::librecreatures {
inline void build_palette_channel_ramp(std::array<uint8_t, 256>& output,
                                int low_intensity,
                                int middle_intensity,
                                int high_intensity) {
    const int middle_to_low_delta = middle_intensity - low_intensity;
    const int middle_to_high_delta = high_intensity - middle_intensity;
    for (int control = 0; control < 0x40; ++control) {
        output[static_cast<size_t>(control)] =
            static_cast<std::uint8_t>((control * low_intensity) / 0x40);
        output[static_cast<size_t>(control) + 0x40] =
            static_cast<std::uint8_t>(low_intensity +
                                      (control * middle_to_low_delta) / 0x40);
        output[static_cast<size_t>(control) + 0x80] =
            static_cast<std::uint8_t>(middle_intensity +
                                      (control * middle_to_high_delta) / 0x40);
        output[static_cast<size_t>(control) + 0xc0] =
            static_cast<std::uint8_t>(high_intensity +
                                      (control * (0xff - high_intensity)) /
                                          0x40);
    }
}

namespace {

inline int centred_control_offset(std::uint8_t control, int scale) {
    const int centred = static_cast<int>(control) - 0x80;
    return (centred * scale) / 0x80;
}

inline int truncated_float_to_int(float value) {

    return static_cast<int>(value);
}

}

template <typename Palette>
inline std::array<uint8_t, 256> build_creature_palette_remap(
    const Palette& palette, uint8_t red_control, uint8_t green_control,
    uint8_t blue_control, uint8_t hue_rotation_control, uint8_t colour_swap_control) {
    if (palette.size() != 256) throw std::invalid_argument("C1 pigment remapping requires a 256-colour palette");
    std::array<uint8_t, 256> output{};
    const int red_offset = centred_control_offset(red_control, 0x14);
    const int green_offset = centred_control_offset(green_control, 0x14);
    const int blue_offset = centred_control_offset(blue_control, 0x14);

    const int red_range = centred_control_offset(red_control, 0x20);
    const int green_range = centred_control_offset(green_control, 0x20);
    const int blue_range = centred_control_offset(blue_control, 0x20);

    std::array<uint8_t, 256> red_ramp{};
    std::array<uint8_t, 256> green_ramp{};
    std::array<uint8_t, 256> blue_ramp{};
    build_palette_channel_ramp(red_ramp, red_offset + 0x40,
                               red_range + 0x80, red_offset + 0xc0);
    build_palette_channel_ramp(green_ramp, green_offset + 0x40,
                               green_range + 0x80, green_offset + 0xc0);
    build_palette_channel_ramp(blue_ramp, blue_offset + 0x40,
                               blue_range + 0x80, blue_offset + 0xc0);

    for (std::size_t index = 0; index < 10; ++index) {
        output[index] =
            static_cast<std::uint8_t>(index);
    }
    for (std::size_t index = 0xf6; index < 0x100; ++index) {
        output[index] =
            static_cast<std::uint8_t>(index);
    }


    const float colour_swap_factor = static_cast<float>(
        std::fabs(static_cast<double>(colour_swap_control) - 127.5) / 127.5);
    const float colour_swap_complement = 1.0f - colour_swap_factor;

    for (std::size_t source_index = 10;
         source_index < 246; ++source_index) {
        const auto& source = palette[source_index];
        const float red = static_cast<float>(
            red_ramp[source.r]);
        const float green = static_cast<float>(
            green_ramp[source.g]);
        const float blue = static_cast<float>(
            blue_ramp[source.b]);

        float hue_factor;
        float transformed_red;
        float transformed_green;
        float transformed_blue;
        if (hue_rotation_control < 0x80) {
            hue_factor = static_cast<float>(
                (127.0 - static_cast<double>(hue_rotation_control)) / 127.0);
            const float unchanged_factor = 1.0f - hue_factor;
            transformed_red = blue * hue_factor + unchanged_factor * red;
            transformed_green = unchanged_factor * green + hue_factor * red;
            transformed_blue = hue_factor * green + unchanged_factor * blue;
        } else {
            hue_factor = static_cast<float>(
                (static_cast<double>(hue_rotation_control) - 128.0) / 127.0);
            const float unchanged_factor = 1.0f - hue_factor;
            transformed_red = hue_factor * green + unchanged_factor * red;
            transformed_green = hue_factor * blue + unchanged_factor * green;
            transformed_blue = hue_factor * red + unchanged_factor * blue;
        }


        const float whole_red =
            static_cast<float>(truncated_float_to_int(transformed_red));
        const float whole_blue =
            static_cast<float>(truncated_float_to_int(transformed_blue));
        const int target_red = truncated_float_to_int(
            whole_blue * colour_swap_factor +
            whole_red * colour_swap_complement);
        const int target_green = truncated_float_to_int(transformed_green);
        const int target_blue = truncated_float_to_int(
            whole_red * colour_swap_factor +
            whole_blue * colour_swap_complement);

        std::size_t nearest_index = 10;
        std::uint32_t minimum_distance =
            std::numeric_limits<std::uint32_t>::max();
        for (std::size_t candidate_index = 10;
             candidate_index < 246; ++candidate_index) {
            const auto& candidate = palette[candidate_index];
            const int red_delta = target_red - static_cast<int>(candidate.r);
            const int green_delta =
                target_green - static_cast<int>(candidate.g);
            const int blue_delta = target_blue - static_cast<int>(candidate.b);
            const std::uint32_t distance = static_cast<std::uint32_t>(
                red_delta * red_delta + green_delta * green_delta +
                blue_delta * blue_delta);
            if (distance < minimum_distance) {
                minimum_distance = distance;
                nearest_index = candidate_index;
            }
        }

        output[source_index] =
            static_cast<std::uint8_t>(nearest_index);
    }

    return output;
}
} // namespace openc1::librecreatures
