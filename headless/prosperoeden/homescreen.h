// SPDX-License-Identifier: GPL-3.0-or-later
// Home screen tiles: every game in the roms folder of the game files folder gets a tile of its own
// on the PS5 home screen, with its name and cover. Choosing one starts ProsperoEden straight into
// that game (autoboot.h); when the game ends, ProsperoEden closes and the home screen is back.
//
// A tile is a small app in /data/prosperoeden/homescreen/tiles/<FAKEnnnnn>/: the tile program
// (headless/homescreen/tile, copied from the app folder's homescreen/), the launch helper payload
// it sends to the ELF loader, sce_sys/param.json (the game's name, a FAKE title ID), its
// sce_sys/icon0.png (the game's cover, 512x512) and rom.txt (the game). ShadowMountPlus 1.7
// (github.com/drakmor/ShadowMountPlus) puts it on the home screen: the tile is added to its
// manual install list through its HTTP API on 127.0.0.1:10101 (docs/api.md), and it accepts FAKE
// title IDs for homebrew (src/sm_gameinfo.c is_supported_game_title_id).
//
// The tiles follow the folder each time the launcher opens and each time the Library has read the
// games: a new game gets a tile, a game that is gone loses its tile, a tile whose cover has
// appeared since (the Library extracts covers) or whose name changed is registered again. Its
// icon is the cover the Library cached, read here (Eden's stb_image reads only JPEG, not those
// TGAs); a tile that got ProsperoEden's icon instead is registered again. A tile deleted from the home screen stays deleted
// (ShadowMountPlus takes it off its list; config/homescreen.json remembers it). Off with
// "home_screen_tiles": false in config/prosperoeden.json. Every step is in stderr.log, in lines
// that start with "[ProsperoEden] home screen:".
#pragma once

namespace Eden::HomeScreen {
// Brings the tiles up to date on a thread of its own; returns at once. A call while one runs makes
// it run once more afterwards.
void SyncInBackground();
} // namespace Eden::HomeScreen
