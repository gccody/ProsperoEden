// SPDX-License-Identifier: GPL-3.0-or-later
// Starting one game for another app: a home launcher writes /data/homelauncher/autoboot.json
//
//   { "version": 1, "rom": "/mnt/ext1/eden/roms/Game.nsp",
//     "return_title_id": "PPSA99009", "created_unix": 1760000000 }
//
// and then starts ProsperoEden. ProsperoEden takes the file away as it opens and starts the game
// the way the Library does (the game's own settings, its updates, DLC and mods all apply). When
// the game ends (Touchpad + L1, or the game ending by itself) with a return_title_id, ProsperoEden
// closes and the app that asked starts itself again from outside (the console refuses starts from
// inside an app; a home launcher does it with a payload that waits for ProsperoEden to end).
// Without the file, ProsperoEden opens as it always does.
//
// A request is followed only when it is complete and less than a minute old, its ROM is in the
// roms folder of the game files folder in use, and the keys and firmware are set up. Otherwise
// the Library opens and says why. A game that fails to start also leaves the player in the
// Library with its error, not in the other app.
#pragma once
#include <string>

namespace Eden::Autoboot {
// Before every launcher: the ROM to start instead of opening it, or empty for the launcher.
// The first time, a request is taken; after a requested game, the app it names is started and
// this process ends (this returns only when that start was refused). launch_error is the
// launcher's message: why the last game did not start (then nothing is started for it), and
// where a request that is not followed says why.
std::string Next(std::string& launch_error);
// The requested game is running and will go back to another app when it ends: a game that ends
// by itself returns to the launcher loop (headless/main.cpp) instead of closing ProsperoEden.
bool Returning() noexcept;
// The game is being stopped. A stop that runs into its limit (stop_limit.h) or a crash while it
// stops starts ProsperoEden again; the note written here lets that new process go back too.
void Leaving() noexcept;
} // namespace Eden::Autoboot
