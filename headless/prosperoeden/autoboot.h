// SPDX-License-Identifier: GPL-3.0-or-later
// Starting one game straight away. A game's home screen tile (homescreen.h) writes
// /data/prosperoeden/homescreen/autoboot.json
//
//   { "version": 1, "rom": "/mnt/ext1/eden/roms/Game.nsp",
//     "return_title_id": "FAKE10001", "created_unix": 1760000000 }
//
// and then has ProsperoEden started. ProsperoEden takes the file away as it opens and starts the
// game the way the Library does (the game's own settings, its updates, DLC and mods all apply).
// When the game ends (Touchpad + L1, or the game ending by itself) and the request names the app
// that asked (return_title_id), ProsperoEden closes: the player is back on the home screen. (The
// console refuses to start a title from inside an app, so going back into an app is that app's
// part, from outside.) Without the file, ProsperoEden opens as it always does.
//
// A request is followed only when it is complete and less than a minute old, its ROM is in the
// roms folder of the game files folder in use, and the keys and firmware are set up. Otherwise
// the Library opens and says why. A game that fails to start also leaves the player in the
// Library with its error.
#pragma once
#include <string>

namespace Eden::Autoboot {
// Before every launcher: the ROM to start instead of opening it, or empty for the launcher.
// The first time, a request is taken; after a requested game that names the app that asked, this
// process ends (this returns only when the system refused to close it). launch_error is the
// launcher's message: why the last game did not start (then the Library opens with it), and
// where a request that is not followed says why.
std::string Next(std::string& launch_error);
// The requested game is running and ProsperoEden closes when it ends: a game that ends by itself
// returns to the launcher loop (headless/main.cpp), which closes the app.
bool Returning() noexcept;
// The game is being stopped. A stop that runs into its limit (stop_limit.h) or a crash while it
// stops starts ProsperoEden again; the note written here makes that new process close too.
void Leaving() noexcept;
} // namespace Eden::Autoboot
