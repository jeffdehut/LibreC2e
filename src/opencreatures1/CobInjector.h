#pragma once

#include <filesystem>
#include <vector>

// Injects Creatures 1 add-on objects (.cob files) and their removers (.rcb files) into
// the world, like the Object Injector applet: the object scripts are added to the
// scriptorium, replacing existing ones, then the install scripts are run.

std::vector<std::filesystem::path> list_injectable_cobs();
void inject_cob(const std::filesystem::path& path);

// -1 denotes unlimited; finite allowances are session/world companion state.
int cob_remaining(const std::filesystem::path& path);
