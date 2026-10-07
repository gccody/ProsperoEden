#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds headless/prosperoeden/autoboot.cpp on the host with tools/ci/autoboot_check.cpp and runs
# every case of README "Autoboot" against a real /data (created here; run on a disposable Linux
# machine or CI runner, it needs sudo once to create /data).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cxx=${CXX:-clang++-18}
work=$(mktemp -d)
trap 'chmod -R u+w /data/prosperoeden/homescreen 2>/dev/null || true; rm -rf "$work"' EXIT

"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/headless" -I"$root/headless/prosperoeden" \
    "$root/tools/ci/autoboot_check.cpp" "$root/headless/prosperoeden/autoboot.cpp" -o "$work/check"
# The home screen tiles' sources as the app builds them (-Wall -Wextra -Werror), checked here in a
# minute rather than at the end of the hour-long PS5 build.
"$cxx" -std=c++20 -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter -I"$root/headless" \
    -I"$root/headless/prosperoeden" -I/usr/include/stb "$root/headless/prosperoeden/homescreen.cpp"
"$cxx" -std=c++20 -fsyntax-only -Wall -Wextra -Werror "$root/headless/homescreen/tile/src/main.cpp"
"${CC:-clang-18}" -std=c11 -fsyntax-only -Wall -Wextra -Werror "$root/headless/homescreen/launch_helper.c"

if [[ ! -w /data ]]; then
    sudo mkdir -p /data
    sudo chown "$(id -u):$(id -g)" /data
fi
files=$work/eden-files
request=/data/prosperoeden/homescreen/autoboot.json
note=/data/prosperoeden/config/autoboot-return.json
failures=0

reset() {
    rm -rf /data/prosperoeden/homescreen /data/prosperoeden "$files"
    mkdir -p /data/prosperoeden/homescreen /data/prosperoeden/config "$files/roms" "$files/keys"
    : > "$files/roms/Game.nsp"
    : > "$files/keys/prod.keys"
    : > "$work/Outside.nsp"
    printf '{"version": 1, "game_files": "%s"}\n' "$files" > /data/prosperoeden/config/prosperoeden.json
}
write_request() { # rom return_title age_seconds [version]
    local created=$(( $(date +%s) - $3 ))
    printf '{"version": %s, "rom": "%s", "return_title_id": "%s", "created_unix": %s}\n' \
        "${4:-1}" "$1" "$2" "$created" > "$request"
}
run() { # name mode -> $out
    out=$("$work/check" "$2" 2>&1) || { echo "FAIL $1: exit $?"; echo "$out"; failures=$((failures + 1)); }
}
expect() { # name pattern
    if ! grep -Fq -- "$2" <<<"$out"; then
        echo "FAIL $1: expected [$2] in:"; echo "$out"; failures=$((failures + 1))
    fi
}
refuse() { # name pattern
    if grep -Fq -- "$2" <<<"$out"; then
        echo "FAIL $1: unexpected [$2] in:"; echo "$out"; failures=$((failures + 1))
    fi
}
gone() { # name path
    if [[ -e $2 ]]; then echo "FAIL $1: $2 still exists"; failures=$((failures + 1)); fi
}
rom="$files/roms/Game.nsp"

reset; run none first
expect none "first ROM=[] ERROR=[] RETURNING=0"; refuse none "autoboot:"; refuse none LAUNCH

reset; write_request "$rom" PPSA99731 0; run return session
expect return "first ROM=[$rom] ERROR=[] RETURNING=1"
expect return "The game ended; closing ProsperoEden (the request came from PPSA99731)"; expect return EXIT; refuse return LAUNCH
gone return "$request"; gone return "$note"

reset; write_request "$rom" "" 0; run library session
expect library "first ROM=[$rom] ERROR=[] RETURNING=0"; expect library "after ROM=[] ERROR=[]"
refuse library LAUNCH; gone library "$request"

reset; write_request "$rom" PPSA99731 120; run stale first
expect stale "ROM=[] ERROR=[Autoboot: request not followed: it was written "; expect stale "s ago (stale)]"
refuse stale LAUNCH; gone stale "$request"

reset; write_request "$rom" PPSA99731 -60; run future first
expect future "it was written -"; expect future "s ago (stale)]"

reset; printf '{"version": 1, "rom": ' > "$request"; run json first
expect json "is not a JSON object"; gone json "$request"

reset; write_request "$rom" PPSA99731 0 2; run version first
expect version "version is not 1"

reset; write_request "$files/roms/Nope.nsp" PPSA99731 0; run missing first
expect missing "ERROR=[Autoboot: request not followed: ROM missing: $files/roms/Nope.nsp"

reset; write_request "$work/Outside.nsp" PPSA99731 0; run outside first
expect outside "the ROM is not an NSP or XCI file in $files/roms"

reset; write_request "$rom" PPSA99008 0; run self first
expect self "return_title_id is not another app's title ID: PPSA99008"

reset; write_request "$rom" PPSA9973 0; run short first
expect short "return_title_id is not another app's title ID: PPSA9973"

reset; write_request "$rom" PPSA99731 0; out=$(SETUP_ERROR="Missing or empty keys/prod.keys" "$work/check" first 2>&1)
expect setup "setup is incomplete: Missing or empty keys/prod.keys"; refuse setup "ROM=[$rom]"

reset; write_request "$rom" PPSA99731 0; run failed failed
expect failed "after ROM=[] ERROR=[Loader status 2."; expect failed "The requested game ended with an error"
expect failed "again ROM=[] ERROR=[Loader status 2."; refuse failed LAUNCH; gone failed "$note"

reset; write_request "$rom" PPSA99731 0; run leave leave
expect leave LEFT
[[ -f $note ]] || { echo "FAIL leave: no return note"; failures=$((failures + 1)); }
run restart first
expect restart "started again while a requested game stopped"
expect restart "The game ended; closing ProsperoEden (the request came from PPSA99731)"; expect restart EXIT; gone restart "$note"

reset; printf '{"version": 1, "return_title_id": "PPSA99731", "created_unix": %s}\n' $(( $(date +%s) - 120 )) > "$note"
run old-note first
expect old-note "Return note not followed: it was written "; refuse old-note LAUNCH
gone old-note "$note"

reset; write_request "$rom" PPSA99731 0; chmod a-w /data/prosperoeden/homescreen; run locked first; chmod u+w /data/prosperoeden/homescreen
expect locked "cannot be removed"; refuse locked "ROM=[$rom]"

if (( failures )); then
    echo "Autoboot check: $failures failure(s)"
    exit 1
fi
echo "Autoboot check PASS: no request, return, Library, stale, future, bad JSON, version, missing ROM,"
echo "ROM outside roms, own and short return IDs, setup, failed game, restart note, old note"
echo "undeletable request (the way back: ProsperoEden closes, no launch from inside the app)"
