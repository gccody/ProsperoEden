// SPDX-License-Identifier: GPL-3.0-or-later
// A Switch game's home screen tile (headless/prosperoeden/homescreen.h). ProsperoEden puts one copy
// of this program in each tile's folder, beside the tile's own sce_sys/param.json (a FAKE title
// ID, the game's name and cover) and rom.txt (the game). Starting the tile:
//
//   1. writes /data/prosperoeden/homescreen/autoboot.json, the request ProsperoEden follows as it
//      opens (headless/prosperoeden/autoboot.h): this game, and this tile as the app that asked,
//      so ProsperoEden closes when the game ends;
//   2. writes the launch helper's job and sends the helper (launch-helper.elf, beside this
//      program) to the ELF loader on TCP 9021: the console refuses to start a title from inside
//      an app, and the helper starts ProsperoEden from outside once this tile has closed;
//   3. closes.
//
// Built with ps5-native-app-boilerplate (headless/homescreen/build.sh), whose applications link
// no C++ library: plain buffers only. Nothing is drawn: the splash shows until the tile closes.
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
// libSceNet, as ps5-native-app-boilerplate's elevation client reaches the ELF loader
// (examples/sandbox-elevation/src/elevation.cpp run_helper).
int sceNetSocket(const char* name, int domain, int type, int protocol);
int sceNetSocketClose(int socket);
int sceNetConnect(int socket, const void* address, std::uint32_t address_length);
int sceNetSend(int socket, const void* data, std::size_t length, int flags);
int sceNetSetsockopt(int socket, int level, int option, const void* value, std::uint32_t size);
// libkernel and libSceSystemService, as the boilerplate's demo and ProsperoEden's
// src/lifecycle.c use them.
int sceKernelSendNotificationRequest(std::uint32_t device, void* request, std::size_t size, int blocking);
int sceKernelUsleep(std::uint32_t microseconds);
int sceSystemServiceLoadExec(const char* path, const char** arguments);
}

namespace {

constexpr const char* kFolder = "/data/prosperoeden/homescreen";
constexpr const char* kRequest = "/data/prosperoeden/homescreen/autoboot.json";
constexpr const char* kJob = "/data/prosperoeden/homescreen/launch-job.txt";
constexpr const char* kLog = "/data/prosperoeden/logs/homescreen-tile.log";
constexpr const char* kProsperoEden = "PPSA99008";

struct NetSockaddrIn {
    std::uint8_t length;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint16_t virtual_port;
    std::uint8_t zero[6];
};

struct NotificationRequest {
    std::uint8_t reserved[45];
    char message[3075];
};

std::FILE* g_log = nullptr;
NotificationRequest g_notification;
char g_text[16384]; // param.json, then the request
char g_rom[4096];
char g_why[512];

void say(const char* what, const char* detail) {
    if (g_log == nullptr) return;
    std::fprintf(g_log, "%lld %s%s\n", static_cast<long long>(std::time(nullptr)), what, detail);
    std::fflush(g_log);
}

void notify(const char* first, const char* second) {
    std::memset(&g_notification, 0, sizeof(g_notification));
    std::snprintf(g_notification.message, sizeof(g_notification.message), "%s%s", first, second);
    (void)sceKernelSendNotificationRequest(0, &g_notification, sizeof(g_notification), 0);
}

[[noreturn]] void close_tile() {
    if (g_log != nullptr) std::fclose(g_log);
    std::fflush(nullptr);
    // Not exit(): a native title that calls it ends in the crash reporter (ProsperoEden
    // src/lifecycle.c).
    (void)sceSystemServiceLoadExec("exit", nullptr);
    for (;;) sceKernelUsleep(100000);
}

// The file's first size-1 bytes, terminated. False when it cannot be read.
bool read_file(const char* path, char* text, std::size_t size) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    const std::size_t count = std::fread(text, 1, size - 1, file);
    text[count] = '\0';
    std::fclose(file);
    return true;
}

// Written in full beside the target, then renamed over it.
bool write_file(const char* path, const char* text) {
    char temporary[256];
    std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    std::FILE* file = std::fopen(temporary, "wb");
    if (file == nullptr) return false;
    const std::size_t length = std::strlen(text);
    bool ok = std::fwrite(text, 1, length, file) == length;
    ok = std::fclose(file) == 0 && ok;
    if (ok && std::rename(temporary, path) == 0) return true;
    std::remove(temporary);
    return false;
}

// "titleId": "FAKE10001" from this tile's own param.json, into title (10 bytes).
bool own_title_id(char* title) {
    title[0] = '\0';
    if (!read_file("/app0/sce_sys/param.json", g_text, sizeof(g_text))) return false;
    const char* key = std::strstr(g_text, "\"titleId\"");
    const char* colon = key == nullptr ? nullptr : std::strchr(key + 9, ':');
    const char* open = colon == nullptr ? nullptr : std::strchr(colon, '"');
    const char* close = open == nullptr ? nullptr : std::strchr(open + 1, '"');
    if (close == nullptr || close - open - 1 != 9) return false;
    std::memcpy(title, open + 1, 9);
    title[9] = '\0';
    return true;
}

// The ROM's path as a JSON string (quotes and backslashes escaped).
void quoted(const char* text, char* out, std::size_t size) {
    std::size_t at = 0;
    out[at++] = '"';
    for (const char* c = text; *c != '\0' && at + 3 < size; ++c) {
        if (*c == '"' || *c == '\\') out[at++] = '\\';
        if (static_cast<unsigned char>(*c) >= 0x20) out[at++] = *c;
    }
    out[at++] = '"';
    out[at] = '\0';
}

bool send_helper(const char* path) {
    const int file = open(path, O_RDONLY);
    if (file < 0) {
        std::snprintf(g_why, sizeof(g_why), "%s cannot be read (%s)", path, std::strerror(errno));
        return false;
    }
    const int socket = sceNetSocket("homescreen_tile", 2, 1, 6);
    if (socket < 0) {
        close(file);
        std::snprintf(g_why, sizeof(g_why), "no socket");
        return false;
    }
    constexpr int kSocketLevel = 0xffff;
    constexpr int kTimeoutUs = 5'000'000;
    // Send and receive timeouts (SO_SNDTIMEO, SO_RCVTIMEO), as the elevation client sets them.
    (void)sceNetSetsockopt(socket, kSocketLevel, 0x1105, &kTimeoutUs, sizeof(kTimeoutUs));
    (void)sceNetSetsockopt(socket, kSocketLevel, 0x1106, &kTimeoutUs, sizeof(kTimeoutUs));
    constexpr std::uint16_t kPort = 9021;
    const NetSockaddrIn loader{sizeof(NetSockaddrIn), 2,
                               static_cast<std::uint16_t>((kPort << 8) | (kPort >> 8)), 0x0100007f, 0, {0}};
    bool sent = sceNetConnect(socket, &loader, sizeof(loader)) >= 0;
    if (!sent) std::snprintf(g_why, sizeof(g_why), "the ELF loader does not answer on port 9021");
    static char buffer[16384];
    while (sent) {
        const ssize_t count = read(file, buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            std::snprintf(g_why, sizeof(g_why), "the helper could not be read");
            sent = false;
            break;
        }
        for (ssize_t done = 0; done < count && sent;) {
            const int wrote = sceNetSend(socket, buffer + done, static_cast<std::size_t>(count - done), 0);
            if (wrote <= 0) {
                std::snprintf(g_why, sizeof(g_why), "the ELF loader stopped taking the helper");
                sent = false;
            } else {
                done += wrote;
            }
        }
    }
    (void)sceNetSocketClose(socket);
    close(file);
    return sent;
}

}  // namespace

int main() {
    (void)mkdir("/data/prosperoeden/logs", 0777);
    g_log = std::fopen(kLog, "a");
    char title[10];
    const bool named = own_title_id(title);
    (void)read_file("/app0/rom.txt", g_rom, sizeof(g_rom));
    for (std::size_t length = std::strlen(g_rom); length > 0 && static_cast<unsigned char>(g_rom[length - 1]) <= ' ';)
        g_rom[--length] = '\0';
    say("tile for ", g_rom);
    if (!named || g_rom[0] == '\0') {
        say("this tile names no game; ProsperoEden makes it again when it opens", "");
        notify("This tile is damaged. Open ProsperoEden once to rebuild the home screen tiles.", "");
        close_tile();
    }

    (void)mkdir(kFolder, 0777);
    const long long now = static_cast<long long>(std::time(nullptr));
    static char rom[4200];
    quoted(g_rom, rom, sizeof(rom));
    std::snprintf(g_text, sizeof(g_text),
                  "{\n  \"version\": 1,\n  \"rom\": %s,\n  \"return_title_id\": \"%s\",\n  \"created_unix\": %lld\n}\n",
                  rom, title, now);
    char job[128];
    std::snprintf(job, sizeof(job), "start=%s\nafter=%s\ncreated=%lld\n", kProsperoEden, title, now);
    bool ok = write_file(kRequest, g_text);
    if (!ok) std::snprintf(g_why, sizeof(g_why), "%s cannot be written (%s)", kRequest, std::strerror(errno));
    if (ok && !write_file(kJob, job)) {
        std::snprintf(g_why, sizeof(g_why), "%s cannot be written (%s)", kJob, std::strerror(errno));
        ok = false;
    }
    ok = ok && send_helper("/app0/launch-helper.elf");
    if (!ok) {
        std::remove(kRequest);
        std::remove(kJob);
        say("not started: ", g_why);
        notify("The game could not be started: ", g_why);
        close_tile();
    }
    say("helper sent; closing so it can start ProsperoEden", "");
    close_tile();
}
