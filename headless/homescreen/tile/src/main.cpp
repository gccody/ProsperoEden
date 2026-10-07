// SPDX-License-Identifier: GPL-3.0-or-later
// A Switch game's home screen tile (headless/homescreen/homescreen.h). ProsperoEden puts one copy
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
// Built with ps5-native-app-boilerplate (headless/homescreen/build.sh). Nothing is drawn: the
// console's splash shows until the tile closes.
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
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

void say(const std::string& line) {
    if (g_log == nullptr) return;
    std::fprintf(g_log, "%lld %s\n", static_cast<long long>(std::time(nullptr)), line.c_str());
    std::fflush(g_log);
}

void notify(const std::string& message) {
    static NotificationRequest request;
    std::memset(&request, 0, sizeof(request));
    std::snprintf(request.message, sizeof(request.message), "%s", message.c_str());
    (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

[[noreturn]] void close_tile() {
    if (g_log != nullptr) std::fclose(g_log);
    std::fflush(nullptr);
    // Not exit(): a native title that calls it ends in the crash reporter (ProsperoEden
    // src/lifecycle.c).
    (void)sceSystemServiceLoadExec("exit", nullptr);
    for (;;) sceKernelUsleep(100000);
}

bool read_file(const char* path, std::string* text, std::size_t limit) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    text->clear();
    char buffer[1024];
    for (std::size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) > 0 && text->size() <= limit;)
        text->append(buffer, count);
    std::fclose(file);
    return text->size() <= limit;
}

// Written in full beside the target, then renamed over it.
bool write_file(const char* path, const std::string& text) {
    const std::string temporary = std::string(path) + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    ok = std::fclose(file) == 0 && ok;
    if (ok && std::rename(temporary.c_str(), path) == 0) return true;
    std::remove(temporary.c_str());
    return false;
}

// "titleId": "FAKE10001" from this tile's own param.json.
std::string own_title_id() {
    std::string param;
    if (!read_file("/app0/sce_sys/param.json", &param, 64 * 1024)) return {};
    const std::size_t key = param.find("\"titleId\"");
    if (key == std::string::npos) return {};
    const std::size_t open = param.find('"', param.find(':', key) + 1);
    const std::size_t close = open == std::string::npos ? open : param.find('"', open + 1);
    if (close == std::string::npos || close - open - 1 != 9) return {};
    return param.substr(open + 1, 9);
}

std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
    }
    return out + "\"";
}

bool send_helper(const char* path, std::string* why) {
    const int file = open(path, O_RDONLY);
    if (file < 0) {
        *why = std::string(path) + " cannot be read (" + std::strerror(errno) + ")";
        return false;
    }
    const int socket = sceNetSocket("homescreen_tile", 2, 1, 6);
    if (socket < 0) {
        close(file);
        *why = "no socket";
        return false;
    }
    constexpr int kSocketLevel = 0xffff;
    constexpr int kTimeoutUs = 5'000'000;
    for (const int option : {0x1105, 0x1106})
        (void)sceNetSetsockopt(socket, kSocketLevel, option, &kTimeoutUs, sizeof(kTimeoutUs));
    constexpr std::uint16_t kPort = 9021;
    const NetSockaddrIn loader{sizeof(NetSockaddrIn), 2,
                               static_cast<std::uint16_t>((kPort << 8) | (kPort >> 8)), 0x0100007f, 0, {0}};
    bool sent = sceNetConnect(socket, &loader, sizeof(loader)) >= 0;
    if (!sent) *why = "the ELF loader does not answer on port 9021";
    char buffer[16384];
    while (sent) {
        const ssize_t count = read(file, buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            *why = "the helper could not be read";
            sent = false;
            break;
        }
        for (ssize_t done = 0; done < count && sent;) {
            const int wrote = sceNetSend(socket, buffer + done, static_cast<std::size_t>(count - done), 0);
            if (wrote <= 0) {
                *why = "the ELF loader stopped taking the helper";
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
    const std::string title = own_title_id();
    std::string rom;
    (void)read_file("/app0/rom.txt", &rom, 4096);
    while (!rom.empty() && static_cast<unsigned char>(rom.back()) <= ' ') rom.pop_back();
    say("tile " + title + " for " + rom);
    if (title.size() != 9 || rom.empty()) {
        say("this tile names no game; ProsperoEden recreates it when it opens");
        notify("This tile is damaged. Open ProsperoEden once to rebuild the home screen tiles.");
        close_tile();
    }

    (void)mkdir(kFolder, 0777);
    const long long now = static_cast<long long>(std::time(nullptr));
    const std::string request = "{\n  \"version\": 1,\n  \"rom\": " + quoted(rom) +
                                ",\n  \"return_title_id\": " + quoted(title) +
                                ",\n  \"created_unix\": " + std::to_string(now) + "\n}\n";
    const std::string job = "start=" + std::string(kProsperoEden) + "\nafter=" + title +
                            "\ncreated=" + std::to_string(now) + "\n";
    std::string why;
    bool ok = write_file(kRequest, request);
    if (!ok) why = std::string(kRequest) + " cannot be written (" + std::strerror(errno) + ")";
    ok = ok && write_file(kJob, job);
    if (!ok && why.empty()) why = std::string(kJob) + " cannot be written (" + std::strerror(errno) + ")";
    ok = ok && send_helper("/app0/launch-helper.elf", &why);
    if (!ok) {
        std::remove(kRequest);
        std::remove(kJob);
        say("not started: " + why);
        notify("The game could not be started: " + why + ".");
        close_tile();
    }
    say("helper sent; closing so it can start ProsperoEden");
    close_tile();
}
