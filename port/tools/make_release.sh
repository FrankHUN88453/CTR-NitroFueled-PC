#!/bin/sh
# Builds the public release zip: the exe WITHOUT embedded game data (build_release.bat); it carries
# the first-start setup that makes shaders.bin from the player's own dump.
# usage: tools/make_release.sh <version>   -> dist/CTR-NitroFueled-PC-<version>.zip
set -e
cd "$(dirname "$0")/.."
V=${1:?version}
powershell -NoProfile -Command "cmd /c \"$(cygpath -w "$(pwd)/build_release.bat")\"" | grep -E "error|FAILED" && exit 1
D=dist/CTR-NitroFueled-PC-$V
rm -rf "$D" "$D.zip"
mkdir -p "$D"
cp build-release/CrashTeamRacingNitroFueled.exe dist_files/README.txt "$D/"
cp ../LICENSE "$D/LICENSE.txt"
# Windows line endings for the text files
for f in "$D/README.txt" "$D/LICENSE.txt"; do sed -i 's/\r*$/\r/' "$f"; done
(cd dist && powershell -NoProfile -Command "Compress-Archive -Force -Path 'CTR-NitroFueled-PC-$V/*' -DestinationPath 'CTR-NitroFueled-PC-$V.zip'")
ls -la "$D.zip"
