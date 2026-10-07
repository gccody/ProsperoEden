// SPDX-License-Identifier: GPL-3.0-or-later
// A home launcher's request to start one game (autoboot.h).
#include "autoboot.h"

#include "assets_dir.h"
#include "diagnostics.h"
#include "metadata_bridge.h"
#include "settings_store.h"
#include "storage_paths.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>

extern "C" {
// src/lifecycle.c: returns only when the system refuses.
int eden_exit_app(void);
}

namespace Eden::Autoboot {
namespace {
using Json = Settings::Json;

constexpr const char* kRequestFile = "/data/homelauncher/autoboot.json";
constexpr const char* kOwnTitle = "PPSA99008";
// A request (and the note a stopping game leaves) is followed for a minute after it was written:
// a file left behind by an app that never started ProsperoEden does not start a game days later.
constexpr long long kMaxAgeSeconds = 60;
constexpr long long kMaxAheadSeconds = 5;
constexpr std::size_t kMaxFileBytes = 4096;

bool first = true;
bool active = false;       // the game running, or the one that just ended, was requested
std::string return_title;  // the app it goes back to; empty: the Library

// Beside the settings: the stopping game's way back, for a process started again (Leaving).
std::string ReturnNote() { return ConfigFile("autoboot-return.json"); }

long long Now() { return static_cast<long long>(std::time(nullptr)); }

std::string Hex(int value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned>(value));
    return text;
}

std::string String(const Json& document, const char* key) {
    if (!document.is_object()) return {};
    const auto at = document.find(key);
    return at != document.end() && at->is_string() ? at->get<std::string>() : std::string{};
}

// Four capital letters and five digits, as PPSA99008.
bool ValidTitleId(std::string_view id) {
    if (id.size() != 9) return false;
    for (std::size_t i = 0; i < id.size(); ++i)
        if (i < 4 ? (id[i] < 'A' || id[i] > 'Z') : (id[i] < '0' || id[i] > '9')) return false;
    return true;
}

// Written less than kMaxAgeSeconds ago; why says otherwise.
bool Fresh(const Json& document, std::string* why) {
    const auto at = document.find("created_unix");
    if (at == document.end() || !at->is_number_integer()) {
        *why = "created_unix is missing";
        return false;
    }
    const long long age = Now() - at->get<long long>();
    if (age <= kMaxAgeSeconds && age >= -kMaxAheadSeconds) return true;
    *why = "it was written " + std::to_string(age) + " s ago (stale)";
    return false;
}

// A ROM directly in the game files folder's roms/, where the Library finds its games: its
// settings, cover and place in the recent games are kept by that file name.
bool InRoms(const std::string& rom) {
    const std::string folder = AssetsPath("roms") + "/";
    return rom.size() > folder.size() && rom.compare(0, folder.size(), folder) == 0 &&
           ValidRomFilename(std::string_view(rom).substr(folder.size()));
}

// The file as a document (discarded when it is not one), and the file is gone: it is followed at
// most once, also when this process ends while following it. *present: there was a file.
Json Take(const std::string& path, bool* present, std::string* why) {
    *present = false;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    // Not there, or (without filesystem access) out of sight: as if there were no request.
    if (!file && (errno == ENOENT || errno == ENOTDIR || !FilesystemAccess())) return Json(Json::value_t::discarded);
    *present = true;
    std::string text;
    bool read = file != nullptr;
    if (file) {
        char buffer[1024];
        for (std::size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() <= kMaxFileBytes;)
            text.append(buffer, count);
        read = !std::ferror(file);
        std::fclose(file);
    }
    if (!read) *why = path + " cannot be read (" + std::strerror(errno) + ")";
    if (std::remove(path.c_str()) != 0) {
        // Followed again at the next start otherwise.
        *why = path + " cannot be removed (" + std::strerror(errno) + ")";
        return Json(Json::value_t::discarded);
    }
    if (!read) return Json(Json::value_t::discarded);
    if (text.size() > kMaxFileBytes) {
        *why = path + " is larger than " + std::to_string(kMaxFileBytes) + " bytes";
        return Json(Json::value_t::discarded);
    }
    Json document = Json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        *why = path + " is not a JSON object";
        return Json(Json::value_t::discarded);
    }
    return document;
}

// Shown in the Library and kept in the log. A crash notice already there stays: the launcher
// shows it in its own way (crash_report.h), and this is in the log.
void Say(std::string& launch_error, const std::string& text) {
    Report("autoboot", text.c_str());
    if (launch_error.empty()) launch_error = "Autoboot: " + text;
}

// The way back: ProsperoEden closes, and the app that asked starts again. The console refuses to
// start a title from inside a running app (0x80940010 on firmware 13.60), so the asking app does
// that from outside: its helper payload waits for this process to end (the home launcher's
// tools/payloads/launch_helper.c). Returns only when the system refused to close the app.
std::string Return(const std::string& title, std::string& launch_error) {
    Report("autoboot", ("Closing ProsperoEden; " + title + " starts again").c_str());
    std::fflush(nullptr);
    const int refused = eden_exit_app();
    Say(launch_error, "ProsperoEden could not close to return to " + title + " (" + Hex(refused) + ")");
    return {};
}

std::string FirstStart(std::string& launch_error) {
    bool present = false;
    std::string why;
    // The process before was stopping a requested game and was started again (stop_limit.h,
    // crash_report.h): the player still goes back.
    const Json note = Take(ReturnNote(), &present, &why);
    if (present) {
        const std::string title = String(note, "return_title_id");
        if (note.is_discarded()) Report("autoboot", ("Return note not followed: " + why).c_str());
        else if (!Fresh(note, &why)) Report("autoboot", ("Return note not followed: " + why).c_str());
        else if (!ValidTitleId(title) || title == kOwnTitle) Report("autoboot", ("Return note names no app: " + title).c_str());
        else {
            Report("autoboot", "ProsperoEden started again while a requested game stopped");
            return Return(title, launch_error);
        }
    }

    const Json request = Take(kRequestFile, &present, &why);
    if (!present) {
        if (!FilesystemAccess())
            Report("autoboot", "No filesystem access: a request in /data/homelauncher cannot be seen");
        return {};
    }
    const std::string rom = String(request, "rom");
    const std::string title = String(request, "return_title_id");
    std::string error;
    if (request.is_discarded()) error = why;
    else if (!request.contains("version") || request["version"] != 1) error = "version is not 1";
    else if (!Fresh(request, &why)) error = why;
    else if (rom.empty()) error = "rom is missing";
    else if (!InRoms(rom)) error = "the ROM is not an NSP or XCI file in " + AssetsPath("roms") + ": " + rom;
    else if (!FileExists(rom)) error = "ROM missing: " + rom;
    else if (!title.empty() && (!ValidTitleId(title) || title == kOwnTitle)) error = "return_title_id is not another app's title ID: " + title;
    else if (const char* setup = eden_startup_error(); setup && *setup) error = std::string{"setup is incomplete: "} + setup;
    if (!error.empty()) {
        Say(launch_error, "request not followed: " + error);
        return {};
    }
    active = true;
    return_title = title;
    Report("autoboot", ("Starting " + rom + ", then " + (title.empty() ? std::string{"the Library"} : title)).c_str());
    return rom;
}
} // namespace

std::string Next(std::string& launch_error) {
    if (std::exchange(first, false)) return FirstStart(launch_error);
    if (!std::exchange(active, false)) return {};
    (void)std::remove(ReturnNote().c_str());
    const std::string title = std::exchange(return_title, {});
    if (!launch_error.empty()) {
        // The game did not start, or failed: its error is shown in the Library.
        Report("autoboot", "The requested game ended with an error; opening the Library");
        return {};
    }
    if (title.empty()) return {};
    return Return(title, launch_error);
}

bool Returning() noexcept { return active && !return_title.empty(); }

void Leaving() noexcept {
    if (!Returning()) return;
    try {
        const Json note = {{"version", 1}, {"return_title_id", return_title}, {"created_unix", Now()}};
        if (!Settings::WriteFile(ReturnNote(), note.dump() + "\n"))
            Report("autoboot", "The return note could not be written; a restart while stopping opens the Library");
    } catch (...) {
        Report("autoboot", "The return note could not be written");
    }
}
} // namespace Eden::Autoboot
