// SPDX-License-Identifier: GPL-3.0-or-later
// Home screen tiles (homescreen.h).
#include "homescreen.h"

#include "assets_dir.h"
#include "diagnostics.h"
#include "native_directory.h"
#include "settings_store.h"
#include "storage_paths.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>

#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

// The PNG writer the launcher's tools use (tools/launcher/stb), private to this file.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../tools/launcher/stb/stb_image_write.h"
#pragma clang diagnostic pop

extern "C" {
// libSceNet, as the elevation client (headless/elevation/elevation.cpp) reaches the ELF loader.
int sceNetSocket(const char* name, int domain, int type, int protocol);
int sceNetSocketClose(int socket);
int sceNetConnect(int socket, const void* address, std::uint32_t address_length);
int sceNetSend(int socket, const void* data, std::size_t length, int flags);
int sceNetRecv(int socket, void* data, std::size_t length, int flags);
int sceNetSetsockopt(int socket, int level, int option, const void* value, std::uint32_t size);
}

namespace Eden::HomeScreen {
namespace {
using Json = Settings::Json;
namespace fs = std::filesystem;

constexpr const char* kRoot = "/data/prosperoeden/homescreen";
constexpr const char* kTiles = "/data/prosperoeden/homescreen/tiles";
constexpr int kFirstNumber = 10001;
constexpr int kIconSide = 512;
constexpr std::uint16_t kApiPort = 10101;

void Say(const std::string& text) { Report("home screen", text.c_str()); }

// ---- ShadowMountPlus's HTTP/JSON API (docs/api.md): POST, one JSON object each way ----

struct NetSockaddrIn {
    std::uint8_t length;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint16_t virtual_port;
    std::uint8_t zero[6];
};

// The response's JSON object, or a discarded value when the call failed.
Json Call(const std::string& route, const Json& body) {
    const std::string payload = body.dump();
    const std::string request = "POST " + route + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                "Content-Type: application/json\r\nConnection: close\r\n"
                                "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;
    const int socket = sceNetSocket("prosperoeden_homescreen", 2, 1, 6);
    if (socket < 0) return Json(Json::value_t::discarded);
    constexpr int kSocketLevel = 0xffff;
    constexpr int kTimeoutUs = 10'000'000;
    for (const int option : {0x1105, 0x1106})
        (void)sceNetSetsockopt(socket, kSocketLevel, option, &kTimeoutUs, sizeof(kTimeoutUs));
    const NetSockaddrIn api{sizeof(NetSockaddrIn), 2,
                            static_cast<std::uint16_t>((kApiPort << 8) | (kApiPort >> 8)), 0x0100007f, 0, {0}};
    std::string response;
    bool ok = sceNetConnect(socket, &api, sizeof(api)) >= 0;
    for (std::size_t done = 0; ok && done < request.size();) {
        const int sent = sceNetSend(socket, request.data() + done, request.size() - done, 0);
        if (sent <= 0) ok = false;
        else done += static_cast<std::size_t>(sent);
    }
    char buffer[8192];
    while (ok && response.size() < (8u << 20)) {
        const int count = sceNetRecv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    (void)sceNetSocketClose(socket);
    const std::size_t body_at = response.find("\r\n\r\n");
    if (!ok || body_at == std::string::npos) return Json(Json::value_t::discarded);
    Json reply = Json::parse(response.substr(body_at + 4), nullptr, false);
    if (!reply.is_object()) return Json(Json::value_t::discarded);
    return reply;
}

bool Succeeded(const Json& reply) {
    return reply.is_object() && reply.contains("status") && reply["status"].is_number() && reply["status"] == 0;
}

std::string Failure(const Json& reply) {
    if (!reply.is_object()) return "no answer";
    return reply.value("error", std::string("status ") + (reply.contains("status") ? reply["status"].dump() : "?"));
}

// ---- the games, named and pictured as the launcher's Library names them ----

// eden_services.cpp HashPath, CoverPath, NamePath, CleanTitle: the covers cache is keyed by file name.
std::uint32_t HashName(const std::string& file) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char byte : file) hash = (hash ^ byte) * 16777619u;
    return hash;
}
std::string Cached(const std::string& file, const char* extension) {
    char name[24];
    std::snprintf(name, sizeof(name), "/%08x.%s", static_cast<unsigned>(HashName(file)), extension);
    return CoversDir() + name;
}
std::string CleanTitle(const std::string& file) {
    std::string title = fs::path(file).stem().string();
    if (title.rfind("[Game] ", 0) == 0) title.erase(0, 7);
    if (const auto tag = title.find_first_of("[("); tag != std::string::npos && tag > 0) title.erase(tag);
    while (!title.empty() && (title.back() == ' ' || title.back() == '_' || title.back() == '-')) title.pop_back();
    return title.empty() ? fs::path(file).stem().string() : title;
}

struct Game {
    std::string file;  // its name in roms/
    std::string name;  // the title the game gives itself, else one from its file name
    std::string cover; // the cached cover; empty without one yet
};

std::vector<Game> CurrentGames(bool* readable) {
    std::vector<Game> games;
    std::error_code error;
    const auto entries = ReadNativeDirectory(AssetsPath("roms"), error);
    *readable = !error;
    for (const auto& entry : entries) {
        const std::string file = entry.path().filename().string();
        if (!ValidRomFilename(file) || !FileExists(AssetsPath("roms/" + file))) continue;
        Game game{file, CleanTitle(file), {}};
        std::string saved;
        if (Settings::ReadFile(Cached(file, "name"), saved)) {
            while (!saved.empty() && static_cast<unsigned char>(saved.back()) <= ' ') saved.pop_back();
            if (!saved.empty()) game.name = saved;
        }
        if (FileExists(Cached(file, "tga"))) game.cover = Cached(file, "tga");
        games.push_back(std::move(game));
    }
    return games;
}

// ---- a tile's folder ----

std::string TileFolder(const std::string& title) { return std::string(kTiles) + "/" + title; }

bool SameFile(const std::string& a, const std::string& b) {
    std::string left;
    std::string right;
    return Settings::ReadFile(a, left) && Settings::ReadFile(b, right) && left == right;
}

bool CopyIfChanged(const std::string& from, const std::string& to) {
    if (SameFile(from, to)) return true;
    std::error_code error;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
    return !error;
}

// A cached cover as RGB, top row first. The covers are the TGAs metadata_bridge.cpp WriteTga
// writes (uncompressed true colour); Eden builds stb_image with STBI_ONLY_JPEG
// (src/common/stb.h), so stbi_load cannot read them.
bool ReadCover(const std::string& bytes, int* width, int* height, std::vector<unsigned char>* rgb) {
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(bytes[at]); };
    if (bytes.size() < 18 || byte(1) != 0 || byte(2) != 2) return false;
    const int w = byte(12) | (byte(13) << 8);
    const int h = byte(14) | (byte(15) << 8);
    const int depth = byte(16) / 8;
    const std::size_t start = 18 + byte(0); // after the image ID
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || (depth != 3 && depth != 4) ||
        bytes.size() < start + static_cast<std::size_t>(w) * h * depth)
        return false;
    const bool top_first = (byte(17) & 0x20) != 0;
    const bool right_first = (byte(17) & 0x10) != 0;
    rgb->resize(static_cast<std::size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t from = start + (static_cast<std::size_t>(top_first ? y : h - 1 - y) * w +
                                              (right_first ? w - 1 - x : x)) * depth;
            unsigned char* to = rgb->data() + (static_cast<std::size_t>(y) * w + x) * 3;
            to[0] = byte(from + 2); // stored BGR(A)
            to[1] = byte(from + 1);
            to[2] = byte(from);
        }
    *width = w;
    *height = h;
    return true;
}

// The cover scaled to the home screen's 512x512 (bilinear), else ProsperoEden's own icon; *from_cover
// tells which it is.
bool WriteIcon(const std::string& cover, const std::string& to, bool* from_cover) {
    *from_cover = false;
    std::string bytes;
    if (!cover.empty() && Settings::ReadFile(cover, bytes) && !bytes.empty()) {
        int width = 0;
        int height = 0;
        std::vector<unsigned char> pixels;
        if (ReadCover(bytes, &width, &height, &pixels)) {
            std::vector<unsigned char> icon(static_cast<std::size_t>(kIconSide) * kIconSide * 3);
            for (int y = 0; y < kIconSide; ++y) {
                const float fy = std::max(0.0f, (y + 0.5f) * height / kIconSide - 0.5f);
                const int y0 = std::min(static_cast<int>(fy), height - 1);
                const int y1 = std::min(y0 + 1, height - 1);
                const float ty = fy - static_cast<float>(y0);
                for (int x = 0; x < kIconSide; ++x) {
                    const float fx = std::max(0.0f, (x + 0.5f) * width / kIconSide - 0.5f);
                    const int x0 = std::min(static_cast<int>(fx), width - 1);
                    const int x1 = std::min(x0 + 1, width - 1);
                    const float tx = fx - static_cast<float>(x0);
                    for (int c = 0; c < 3; ++c) {
                        const auto at = [&](int px, int py) {
                            return static_cast<float>(pixels[(static_cast<std::size_t>(py) * width + px) * 3 + c]);
                        };
                        const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                        const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                        icon[(static_cast<std::size_t>(y) * kIconSide + x) * 3 + c] =
                            static_cast<unsigned char>(std::clamp(top + (bottom - top) * ty + 0.5f, 0.0f, 255.0f));
                    }
                }
            }
            const std::string staged = to + ".tmp";
            if (stbi_write_png(staged.c_str(), kIconSide, kIconSide, 3, icon.data(), kIconSide * 3) != 0 &&
                std::rename(staged.c_str(), to.c_str()) == 0) {
                *from_cover = true;
                return true;
            }
            std::remove(staged.c_str());
            Say(cover + ": the icon could not be written; ProsperoEden's is used");
        } else {
            Say(cover + ": not a cover this can read; ProsperoEden's icon is used");
        }
    }
    return CopyIfChanged(AppFile("sce_sys/icon0.png"), to);
}

std::string ParamJson(const std::string& title, const std::string& name) {
    const std::string number = title.substr(4);
    const Json param = {
        {"ageLevel", {{"default", 0}}},
        {"applicationCategoryType", 0},
        {"applicationDrmType", "free"},
        {"attribute", 0},
        {"attribute2", 0},
        {"attribute3", 0},
        {"conceptId", number},
        {"contentBadgeType", 1},
        {"contentId", "UP9000-" + title + "_00-EDENHOMETILE0001"},
        {"contentVersion", "01.000.000"},
        {"downloadDataSize", 0},
        {"gameIntent", {{"permittedIntents", Json::array({{{"intentType", "launchActivity"}}})}}},
        {"localizedParameters", {{"defaultLanguage", "en-US"}, {"en-US", {{"titleName", name}}}}},
        {"masterVersion", "01.00"},
        {"requiredSystemSoftwareVersion", "0x0000000000000000"},
        {"sdkVersion", "0x0000000000000000"},
        {"titleId", title},
        {"versionFileUri", ""},
    };
    return param.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

// The program files every tile carries, from the app folder (refreshed when ProsperoEden is).
bool WriteProgram(const std::string& folder) {
    std::error_code error;
    fs::create_directories(folder + "/sce_module", error);
    return CopyIfChanged(AppFile("homescreen/eboot.bin"), folder + "/eboot.bin") &&
           CopyIfChanged(AppFile("homescreen/libc.prx"), folder + "/sce_module/libc.prx") &&
           CopyIfChanged(AppFile("homescreen/launch-helper.elf"), folder + "/launch-helper.elf");
}

// *with_cover: whether its icon is the game's cover.
bool WriteTile(const std::string& title, const Game& game, bool* with_cover) {
    const std::string folder = TileFolder(title);
    std::error_code error;
    fs::create_directories(folder + "/sce_sys", error);
    (void)chmod(folder.c_str(), 0777);
    return WriteProgram(folder) && Settings::WriteFile(folder + "/sce_sys/param.json", ParamJson(title, game.name)) &&
           WriteIcon(game.cover, folder + "/sce_sys/icon0.png", with_cover) &&
           Settings::WriteFile(folder + "/rom.txt", AssetsPath("roms/" + game.file) + "\n");
}

// Whether a tile carries ProsperoEden's icon: the first tiles were recorded with their cover when it
// could not be read.
bool IconIsProsperoEdens(const std::string& title) {
    return SameFile(TileFolder(title) + "/sce_sys/icon0.png", AppFile("sce_sys/icon0.png"));
}

// ---- the record of tiles: config/homescreen.json ----

struct Tile {
    std::string file;
    std::string title;
    std::string name;
    bool cover = false;           // its icon is the game's cover, not ProsperoEden's
    bool removed_by_user = false; // deleted from the home screen: not made again
};

std::string RecordFile() { return ConfigFile("homescreen.json"); }

std::vector<Tile> LoadRecord() {
    std::vector<Tile> tiles;
    std::string text;
    if (!Settings::ReadFile(RecordFile(), text)) return tiles;
    const Json record = Json::parse(text, nullptr, false);
    if (!record.is_object() || !record.contains("tiles") || !record["tiles"].is_array()) return tiles;
    for (const Json& entry : record["tiles"]) {
        if (!entry.is_object()) continue;
        Tile tile;
        tile.file = entry.value("file", std::string());
        tile.title = entry.value("title_id", std::string());
        tile.name = entry.value("name", std::string());
        tile.cover = entry.value("cover", false);
        tile.removed_by_user = entry.value("removed_by_user", false);
        if (ValidRomFilename(tile.file) && tile.title.size() == 9 && tile.title.rfind("FAKE", 0) == 0)
            tiles.push_back(std::move(tile));
    }
    return tiles;
}

bool SaveRecord(const std::vector<Tile>& tiles) {
    Json list = Json::array();
    for (const Tile& tile : tiles)
        list.push_back({{"file", tile.file}, {"title_id", tile.title}, {"name", tile.name},
                        {"cover", tile.cover}, {"removed_by_user", tile.removed_by_user}});
    const Json record = {{"version", 1}, {"tiles", list}};
    return Settings::WriteFile(RecordFile(), record.dump(2, ' ', false, Json::error_handler_t::replace) + "\n");
}

// ---- the sync ----

void Unregister(const std::string& title) {
    const Json uninstall = Call("/api/v1/games/uninstall", {{"title_id", title}});
    if (!Succeeded(uninstall)) Say(title + ": uninstall not accepted (" + Failure(uninstall) + ")");
    const Json removed = Call("/api/v1/manual/remove", {{"path", TileFolder(title)}});
    if (!Succeeded(removed)) Say(title + ": not taken off the manual list (" + Failure(removed) + ")");
}

void Sync() {
    if (!FilesystemAccess()) return;
    if (!Settings::Bool(Settings::Load(SettingsFile()), Json::json_pointer("/home_screen_tiles"), true)) {
        Say("off (home_screen_tiles is false)");
        return;
    }
    for (const char* part : {"homescreen/eboot.bin", "homescreen/libc.prx", "homescreen/launch-helper.elf"})
        if (!FileExists(AppFile(part))) {
            Say(std::string("this package has no ") + part + "; tiles not updated");
            return;
        }
    const Json version = Call("/api/v1/version", Json::object());
    if (!Succeeded(version)) {
        Say("ShadowMountPlus does not answer on 127.0.0.1:10101 (ShadowMountPlus 1.7 with its API is needed); "
            "tiles not updated");
        return;
    }
    bool readable = false;
    const std::vector<Game> games = CurrentGames(&readable);
    if (!readable) {
        Say("the roms folder cannot be read; tiles not updated");
        return;
    }
    std::vector<Tile> tiles = LoadRecord();
    std::set<std::string> listed; // ShadowMountPlus's manual list
    const Json manual = Call("/api/v1/manual/list", Json::object());
    if (!Succeeded(manual) || !manual.contains("paths") || !manual["paths"].is_array()) {
        Say("ShadowMountPlus's manual list cannot be read (" + Failure(manual) + "); tiles not updated");
        return;
    }
    for (const Json& path : manual["paths"])
        if (path.is_string()) listed.insert(path.get<std::string>());
    std::set<std::string> used; // title IDs the console knows, and the record's
    const Json known = Call("/api/v1/games", {{"include_size", false}});
    if (Succeeded(known) && known.contains("games") && known["games"].is_array())
        for (const Json& game : known["games"])
            if (game.is_object() && game.contains("title_id") && game["title_id"].is_string())
                used.insert(game["title_id"].get<std::string>());
    for (const Tile& tile : tiles) used.insert(tile.title);

    std::error_code error;
    fs::create_directories(kTiles, error);
    (void)chmod(kRoot, 0777);
    (void)chmod(kTiles, 0777);
    bool changed = false;
    int added = 0;
    int removed = 0;
    int refreshed = 0;
    std::vector<Tile> next;
    for (Tile& tile : tiles) {
        const auto game = std::find_if(games.begin(), games.end(), [&](const Game& g) { return g.file == tile.file; });
        const std::string folder = TileFolder(tile.title);
        if (game == games.end()) {
            // The game is gone: so is its tile (also one deleted from the home screen before).
            if (!tile.removed_by_user) Unregister(tile.title);
            fs::remove_all(folder, error);
            ++removed;
            changed = true;
            continue;
        }
        if (tile.removed_by_user) {
            next.push_back(tile);
            continue;
        }
        if (!listed.count(folder)) {
            // ShadowMountPlus took it off its list: the tile was deleted from the home screen.
            Say(tile.title + " (" + tile.name + ") was deleted from the home screen; it stays deleted");
            tile.removed_by_user = true;
            fs::remove_all(folder, error);
            next.push_back(tile);
            changed = true;
            continue;
        }
        if (tile.cover && IconIsProsperoEdens(tile.title)) tile.cover = false;
        const bool better_icon = !tile.cover && !game->cover.empty();
        if (better_icon || tile.name != game->name) {
            // The home screen keeps what it took at registration: register it again.
            bool with_cover = false;
            if (WriteTile(tile.title, *game, &with_cover) && (with_cover || tile.name != game->name)) {
                Unregister(tile.title);
                const Json add = Call("/api/v1/manual/add", {{"path", folder}});
                if (!Succeeded(add)) Say(tile.title + ": not listed again (" + Failure(add) + ")");
                tile.name = game->name;
                tile.cover = with_cover;
                ++refreshed;
                changed = true;
            }
        } else if (!WriteProgram(folder)) {
            Say(tile.title + ": its program could not be refreshed");
        }
        next.push_back(tile);
    }
    int number = kFirstNumber;
    for (const Game& game : games) {
        if (std::any_of(next.begin(), next.end(), [&](const Tile& t) { return t.file == game.file; })) continue;
        char title[16];
        do {
            std::snprintf(title, sizeof(title), "FAKE%05d", number++);
        } while (used.count(title) && number <= 99999);
        if (number > 99999) break;
        used.insert(title);
        bool with_cover = false;
        if (!WriteTile(title, game, &with_cover)) {
            Say(std::string(title) + " (" + game.name + "): the tile could not be written");
            fs::remove_all(TileFolder(title), error);
            continue;
        }
        const Json add = Call("/api/v1/manual/add", {{"path", TileFolder(title)}});
        if (!Succeeded(add)) {
            Say(std::string(title) + " (" + game.name + "): ShadowMountPlus did not list it (" + Failure(add) + ")");
            fs::remove_all(TileFolder(title), error);
            continue;
        }
        next.push_back(Tile{game.file, title, game.name, with_cover, false});
        ++added;
        changed = true;
    }
    if (changed) {
        if (!SaveRecord(next)) Say("config/homescreen.json could not be written");
        (void)Call("/api/v1/scan", {{"reset_attempts", false}});
    }
    Say(std::to_string(games.size()) + " games: " + std::to_string(added) + " tiles added, " +
        std::to_string(refreshed) + " registered again, " + std::to_string(removed) + " removed");
}

std::atomic<bool> busy{false};  // a sync thread exists
std::atomic<bool> again{false}; // a sync was asked for and has not started yet

} // namespace

void SyncInBackground() {
    again = true;
    bool idle = false;
    if (!busy.compare_exchange_strong(idle, true)) return; // the running thread picks it up
    std::thread([] {
        for (;;) {
            while (again.exchange(false)) {
                try {
                    Sync();
                } catch (const std::exception& error) {
                    Say(std::string("stopped: ") + error.what());
                }
            }
            busy = false;
            // A request that came in after the last look, while this thread was ending.
            bool idle = false;
            if (!again.load() || !busy.compare_exchange_strong(idle, true)) return;
        }
    }).detach();
}

} // namespace Eden::HomeScreen
