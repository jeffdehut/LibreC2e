#include "WorldCorrections.h"
#include "EngineContext.h"
#include "Scriptorium.h"
#include "objects/ObjectManager.h"
#include "../../third_party/librecreatures/C1WorldScriptFixes.h"
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace openc1::librecreatures;

bool matches(const std::string& text, uint32_t length, uint64_t hash) {
    return text.size() == length && script_fingerprint(text) == hash;
}

// the corrected script, rebuilt from the world's own original scripts
std::optional<std::string> rebuild(const StockScriptFix& fix, const std::vector<std::string>& current) {
    std::string out;
    for (size_t i = 0; i < fix.piece_count; ++i) {
        const auto& piece = fix.pieces[i];
        if (piece.source < 0) {
            out += piece.text;
            continue;
        }
        const auto& from = current[static_cast<size_t>(piece.source)];
        const auto& source_fix = kStockScriptFixes[piece.source];
        if (!matches(from, source_fix.stock_length, source_fix.stock_hash) || piece.offset + piece.length > from.size()) {
            return std::nullopt;
        }
        out.append(from, piece.offset, piece.length);
    }
    if (!matches(out, fix.fixed_length, fix.fixed_hash)) {
        return std::nullopt;
    }
    return out;
}
} // namespace

void apply_world_corrections() {
    // the world's scripts as they are now, one per fix
    std::vector<std::string> current;
    for (const auto& fix : kStockScriptFixes) {
        current.push_back(g_engine_context.scriptorium->get(fix.family, fix.genus, fix.species, fix.event));
    }
    auto is_stock = [&](size_t i) { return matches(current[i], kStockScriptFixes[i].stock_length, kStockScriptFixes[i].stock_hash); };
    auto is_fixed = [&](size_t i) { return matches(current[i], kStockScriptFixes[i].fixed_length, kStockScriptFixes[i].fixed_hash); };
    // The boat's activation and timer scripts form one change. Preserve all three
    // when a world author customized any one of them.
    bool boat_compatible = true;
    for (size_t i = 0; i < std::size(kStockScriptFixes); ++i) {
        if (kStockScriptFixes[i].family == 3) {
            boat_compatible &= is_stock(i) || is_fixed(i);
        }
    }
    bool drum_fixed = false;
    std::vector<std::optional<std::string>> replacements(std::size(kStockScriptFixes));
    for (size_t i = 0; i < std::size(kStockScriptFixes); ++i) {
        const auto& fix = kStockScriptFixes[i];
        if (fix.family == 3 && !boat_compatible) continue;
        if (is_stock(i)) {
            replacements[i] = rebuild(fix, current);
        }
        if (fix.family == 2 && (is_stock(i) || is_fixed(i))) drum_fixed = true;
    }
    // replaced only once every new script is built, since they're built from the old ones
    for (size_t i = 0; i < std::size(kStockScriptFixes); ++i) {
        if (const auto& text = replacements[i]) {
            const auto& fix = kStockScriptFixes[i];
            g_engine_context.scriptorium->remove(fix.family, fix.genus, fix.species, fix.event);
            g_engine_context.scriptorium->add(fix.family, fix.genus, fix.species, fix.event, *text);
        }
    }
    for (auto* obj : *g_engine_context.objects) {
        if (obj->family == 2 && obj->genus == 8 && obj->species == 15) obj->attr |= ATTR_INVISIBLE;
        if (drum_fixed && obj->family == 2 && obj->genus == 9 && obj->species == 2) obj->actv = ACTV_INACTIVE;
    }
}
