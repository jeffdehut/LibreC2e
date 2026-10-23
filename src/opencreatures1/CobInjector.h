#pragma once

#include <filesystem>
#include <vector>

// Injects Creatures 1 add-on objects (.cob files) and their removers (.rcb files) into
// the world, like the Object Injector applet: the object scripts are added to the
// scriptorium, replacing existing ones, then the install scripts are run.

std::vector<std::filesystem::path> list_injectable_cobs();
void inject_cob(const std::filesystem::path& path);
// Takes an add-on out of the world: by its .rcb when it has one, otherwise by killing the
// objects it makes and removing the scripts it added (LibreCreatures' Injector Kit).
void remove_cob(const std::filesystem::path& path);

// -1 denotes unlimited; finite allowances are session/world companion state.
int cob_remaining(const std::filesystem::path& path);
// The modern layout doesn't count injections (the interface keeps this in step); the classic
// one keeps each add-on's own allowance, as the original's Object Injector does.
extern bool g_cob_unlimited;
