// Adapted from h4rm0n1c/LibreCreatures (GNU GPL version 3).
// b775a03, src/kits/c1kitlib/include/c1kit/cob.hpp. See NOTICE.md and LICENSE.
//
// What an add-on (.cob) does, read from its scripts, for the Injector Kit's Analysis page:
// the objects it makes, the chemicals it changes, things to warn about, and a removal for one
// that came without an .rcb. "Object scripts" are the `scrp` scripts it puts in the
// scriptorium; "install scripts" are the ones run when it's injected.
#pragma once
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>
#include "C1CobHeader.h"

namespace openc1::librecreatures {

// a script's words, split at spaces and commas; [bracketed text] is one word
inline std::vector<std::string> caos_tokens(const std::string& script) {
    std::vector<std::string> tokens;
    std::string word;
    bool bracket = false;
    for (const char c : script) {
        if (bracket) {
            word.push_back(c);
            if (c == ']') bracket = false;
            continue;
        }
        if (c == '[') {
            bracket = true;
            word.push_back(c);
        } else if (c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n') {
            if (!word.empty()) tokens.push_back(word);
            word.clear();
        } else {
            word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (!word.empty()) tokens.push_back(word);
    return tokens;
}

inline bool caos_integer(const std::string& token, int& out) {
    if (token.empty()) return false;
    std::size_t at = (token[0] == '-' || token[0] == '+') ? 1 : 0;
    if (at == token.size()) return false;
    for (std::size_t i = at; i < token.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(token[i]))) return false;
    }
    out = std::atoi(token.c_str());
    return true;
}

inline bool same(const Classifier& a, const Classifier& b) {
    return a.family == b.family && a.genus == b.genus && a.species == b.species;
}

// the classifier and event of an object script's `scrp f g s e` header, as its author wrote it
inline bool script_header(const std::string& script, InstalledScript& out) {
    const std::vector<std::string> t = caos_tokens(script);
    if (t.size() < 5 || t[0] != "scrp") return false;
    return caos_integer(t[1], out.classifier.family) && caos_integer(t[2], out.classifier.genus) &&
           caos_integer(t[3], out.classifier.species) && caos_integer(t[4], out.event);
}

// the objects the install scripts make: their `setv clas <n>`, n being
// family << 24 | genus << 16 | species << 8
inline std::vector<Classifier> created_classifiers(const std::vector<std::string>& install_scripts) {
    std::vector<Classifier> found;
    for (const std::string& script : install_scripts) {
        const std::vector<std::string> t = caos_tokens(script);
        for (std::size_t i = 0; i + 2 < t.size(); ++i) {
            int value = 0;
            if (t[i] == "setv" && t[i + 1] == "clas" && caos_integer(t[i + 2], value)) {
                const Classifier c{(value >> 24) & 0xff, (value >> 16) & 0xff, (value >> 8) & 0xff};
                bool seen = false;
                for (const Classifier& f : found) seen = seen || same(f, c);
                if (!seen) found.push_back(c);
            }
        }
    }
    return found;
}

// The chemicals the scripts change: `chem <n> <amount>`, and the chemical and amount pairs of
// a stimulus (`stim writ <target> a b c d c1 n1 c2 n2 c3 n3 c4 n4`, and `stim shou`, `sign` and
// `tact` without the target). In the order first seen; 1-255.
inline std::vector<int> affected_chemicals(const std::vector<std::string>& object_scripts, const std::vector<std::string>& install_scripts) {
    std::vector<int> found;
    const auto add = [&found](int chemical) {
        if (chemical < 1 || chemical > 255) return;
        for (const int f : found) if (f == chemical) return;
        found.push_back(chemical);
    };
    std::vector<std::string> scripts = object_scripts;
    scripts.insert(scripts.end(), install_scripts.begin(), install_scripts.end());
    for (const std::string& script : scripts) {
        const std::vector<std::string> t = caos_tokens(script);
        for (std::size_t i = 0; i < t.size(); ++i) {
            int value = 0;
            if (t[i] == "chem" && (i == 0 || t[i - 1] != "putv") && i + 1 < t.size() && caos_integer(t[i + 1], value)) {
                add(value);
            } else if (t[i] == "stim" && i + 1 < t.size()) {
                std::size_t at = i + 2;
                if (t[i + 1] == "writ") ++at; // the target
                at += 4; // four stimulus values
                for (int pair = 0; pair < 4 && at < t.size(); ++pair, at += 2) {
                    if (!caos_integer(t[at], value)) break;
                    add(value);
                }
            }
        }
    }
    return found;
}

// whether injecting it needs a creature chosen: an install script names `norn`
inline bool needs_creature(const std::vector<std::string>& install_scripts) {
    for (const std::string& script : install_scripts) {
        for (const std::string& token : caos_tokens(script)) {
            if (token == "norn") return true;
        }
    }
    return false;
}

struct CobWarnings {
    bool kills_creature = false; // `kill norn`
    bool fires_neurons = false; // `fire` or `trig`
    bool makes_nothing = false; // no install scripts: it only adds scripts
};
inline CobWarnings cob_warnings(const std::vector<std::string>& object_scripts, const std::vector<std::string>& install_scripts) {
    CobWarnings w;
    std::vector<std::string> scripts = object_scripts;
    scripts.insert(scripts.end(), install_scripts.begin(), install_scripts.end());
    for (const std::string& script : scripts) {
        const std::vector<std::string> t = caos_tokens(script);
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == "kill" && i + 1 < t.size() && t[i + 1] == "norn") w.kills_creature = true;
            if (t[i] == "fire" || t[i] == "trig") w.fires_neurons = true;
        }
    }
    w.makes_nothing = install_scripts.empty();
    return w;
}

// A removal for an add-on without an .rcb: kill every object it makes, then take out every
// script it adds. Empty when it makes and adds nothing.
inline std::string generated_removal(const std::vector<std::string>& object_scripts, const std::vector<std::string>& install_scripts) {
    std::string script;
    for (const Classifier& c : created_classifiers(install_scripts)) {
        script += "enum " + std::to_string(c.family) + " " + std::to_string(c.genus) + " " + std::to_string(c.species) + ",kill targ,next,";
    }
    for (const std::string& object_script : object_scripts) {
        InstalledScript s{};
        if (script_header(object_script, s)) {
            script += "scrx " + std::to_string(s.classifier.family) + " " + std::to_string(s.classifier.genus) + " " +
                      std::to_string(s.classifier.species) + " " + std::to_string(s.event) + ",";
        }
    }
    return script.empty() ? std::string() : "inst," + script + "endm";
}

} // namespace openc1::librecreatures
