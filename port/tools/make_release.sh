#!/bin/sh
# Builds the public release zip: the exe WITHOUT embedded game data (build_release.bat), setup.bat
# and the Python setup tools that make shaders.bin from the player's own dump.
# usage: tools/make_release.sh <version>   -> dist/CTR-NitroFueled-PC-<version>.zip
set -e
cd "$(dirname "$0")/.."
V=${1:?version}
powershell -NoProfile -Command "cmd /c \"$(cygpath -w "$(pwd)/build_release.bat")\"" | grep -E "error|FAILED" && exit 1
D=dist/CTR-NitroFueled-PC-$V
rm -rf "$D" "$D.zip"
mkdir -p "$D/setup"
cp build-release/CrashTeamRacingNitroFueled.exe "$D/"
cp dist_files/setup.bat dist_files/README.txt "$D/"
cp ../LICENSE "$D/LICENSE.txt"
for f in setup_shaders.py runtime_shaders.json iga.py igz.py export_shaders.py gcn.py gcn2hlsl.py \
         pack_shaders.py font_glyphs.py make_glyph_atlas.py; do
    cp "../tools/$f" "$D/setup/"
done
# Windows line endings for the batch file and the text files
for f in "$D/setup.bat" "$D/README.txt" "$D/LICENSE.txt"; do sed -i 's/\r*$/\r/' "$f"; done
(cd dist && powershell -NoProfile -Command "Compress-Archive -Force -Path 'CTR-NitroFueled-PC-$V' -DestinationPath 'CTR-NitroFueled-PC-$V.zip'")
ls -la "$D.zip"
