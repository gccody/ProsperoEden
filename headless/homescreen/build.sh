#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds what ProsperoEden copies into each home screen tile (headless/homescreen/homescreen.h):
# the tile program (tile/, an app built by ps5-native-app-boilerplate's own build, pointed at it)
# and the launch helper payload (launch_helper.c, built with the boilerplate's Payload SDK as
# headless/self_update_helper is). Output: build/homescreen/{eboot.bin,libc.prx,launch-helper.elf}.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
template="$root/../ps5-native-app-boilerplate"
output="$root/build/homescreen"
mkdir -p "$output"

# The boilerplate builds applications from paths inside its own tree; its .local/ is ignored there.
stage="$template/.local/prosperoeden-tile"
rm -rf "$stage"
mkdir -p "$stage"
cp -a "$root/headless/homescreen/tile/src" "$root/headless/homescreen/tile/sce_sys" "$stage/"
(cd "$template" && APP_SOURCE_DIR=.local/prosperoeden-tile/src \
    APP_PARAM=.local/prosperoeden-tile/sce_sys/param.json \
    APP_SCE_SYS=.local/prosperoeden-tile/sce_sys APP_ASSETS= APP_LAPY_HELPER=0 \
    bash tools/build.sh Folder)
title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' \
    "$stage/sce_sys/param.json")
cp "$template/dist/$title/eboot.bin" "$output/eboot.bin"
cp "$template/dist/$title/sce_module/libc.prx" "$output/libc.prx"

sdk="$template/.deps/native/ps5-payload-sdk"
"$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -o "$output/launch-helper.elf" \
    "$root/headless/homescreen/launch_helper.c" -lSceSystemService -lSceUserService
python3 "$root/tools/validate-loader-elf.py" "$output/launch-helper.elf"
sha256sum "$output/eboot.bin" "$output/libc.prx" "$output/launch-helper.elf"
