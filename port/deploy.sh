#!/bin/sh
# Installs the built game executable next to the PS4 game dump (eboot.bin), so that it starts
# with a double click. The translated shaders are embedded in the exe.
set -e
cd "$(dirname "$0")"
GAME=${1:-F:/PS4/CRTNF/UP0002-CUSA13795_00-CRASHTEAMRACING1}
[ -f "$GAME/eboot.bin" ] || { echo "no eboot.bin in $GAME"; exit 1; }
cp build/CrashTeamRacingNitroFueled.exe "$GAME/"
rm -rf "$GAME/shaders"  # older installs kept them in a folder
echo "installed to $GAME"
