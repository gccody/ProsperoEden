// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of headless/prosperoeden/autoboot.cpp (tools/ci/check-autoboot.sh): the module
// built on its own, with the console calls it makes replaced by ones that print what they were
// given. One scenario per process, as the module keeps "first start" per process.
#include "prosperoeden/autoboot.h"
#include "storage_paths.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

extern "C" {
const char* eden_startup_error(void) {
    const char* error = std::getenv("SETUP_ERROR");
    return error ? error : "";
}
std::uint64_t eden_game_title_id(const char*) { return 0; }
int sceUserServiceInitialize(const void*) { return 0; }
int sceUserServiceGetForegroundUser(int* user) {
    *user = 0x10000001;
    return 0;
}
int sceSystemServiceLaunchApp(const char* title_id, const char** argv, void* context) {
    const auto* words = static_cast<const std::uint32_t*>(context);
    std::printf("LAUNCH %s argv0=%s size=0x%x user=0x%x\n", title_id, argv && argv[0] ? "set" : "null",
                words[0], words[1]);
    const char* rc = std::getenv("LAUNCH_RC");
    return rc ? static_cast<int>(std::strtol(rc, nullptr, 0)) : 0;
}
int eden_exit_app(void) {
    std::printf("EXIT\n");
    std::fflush(stdout);
    _exit(0);
}
}

namespace {
void Show(const char* step, const std::string& rom, const std::string& error) {
    std::printf("%s ROM=[%s] ERROR=[%s] RETURNING=%d\n", step, rom.c_str(), error.c_str(),
                Eden::Autoboot::Returning() ? 1 : 0);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv) {
    Eden::FilesystemAccessStatus() = 0;
    const std::string mode = argc > 1 ? argv[1] : "";
    std::string error;
    std::string rom = Eden::Autoboot::Next(error);
    Show("first", rom, error);
    if (mode == "first") return 0;
    if (mode == "leave") {
        // The game is being stopped and the process is killed by the stop limit: the note stays.
        Eden::Autoboot::Leaving();
        std::printf("LEFT\n");
        return 0;
    }
    if (mode == "session") {
        // Touchpad + L1 (or the game ending by itself): the next launcher turn goes back.
        Eden::Autoboot::Leaving();
        rom = Eden::Autoboot::Next(error);
        Show("after", rom, error);
        return 0;
    }
    if (mode == "failed") {
        // The game did not start: its error goes to the Library, nothing else is started.
        Eden::Autoboot::Leaving();
        error = "Loader status 2. Check this ROM, its keys and firmware; see stderr.log.";
        rom = Eden::Autoboot::Next(error);
        Show("after", rom, error);
        rom = Eden::Autoboot::Next(error);
        Show("again", rom, error);
        return 0;
    }
    return 2;
}
