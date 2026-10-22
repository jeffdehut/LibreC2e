// Adapted from h4rm0n1c/LibreCreatures (GNU GPL version 3).
// 907b8642f79c2688bd8a1c3f43e401a5f9431c8c,
// src/kits/c1kitlib/include/c1kit/cob.hpp. See NOTICE.md and LICENSE.
#pragma once
#include <cstdint>
#include <string>
namespace openc1::librecreatures {
struct Classifier { int family, genus, species; };
struct InstalledScript { Classifier classifier; int event; };
inline bool original_script_classifier(const std::string& script, InstalledScript& out) {
    if (script.size() < 5 || (script.compare(0, 4, "scrp") != 0 && script.compare(0, 4, "scrx") != 0)) {
        return false;
    }
    std::size_t at = 5;  // the command's four characters and its separator
    int values[4] = {};
    for (int& value : values) {
        if (at >= script.size() || script[at] >= ':') return false;  // a variable
        std::uint32_t number = 0;
        if (script[at] == '-') {
            ++at;
            if (at < script.size()) number = 0u - static_cast<std::uint32_t>(script[at++] - '0');
            while (at < script.size()) {
                const char digit = script[at++];
                if (digit < '0') break;
                number = number * 10u - static_cast<std::uint32_t>(digit - '0');
            }
        } else {
            number = static_cast<std::uint32_t>(script[at++] - '0');
            while (at < script.size()) {
                const char digit = script[at++];
                if (digit < '0') break;
                number = number * 10u + static_cast<std::uint32_t>(digit - '0');
            }
        }
        value = static_cast<int>(number & 0xffu);
    }
    out.classifier = {values[0], values[1], values[2]};
    out.event = values[3];
    return true;
}
}
