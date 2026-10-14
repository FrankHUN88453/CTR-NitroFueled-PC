#!/bin/sh
# Builds the Linux AppImage from a Linux build of the runner (CI: .github/workflows/linux.yml).
#   usr/bin/CrashTeamRacingNitroFueled   the runner (RUNPATH $ORIGIN/../lib)
#   usr/bin/vkd3d-compiler               HLSL -> DXBC for the first-start shader setup
#   usr/lib/                             DXVK native (D3D11 on Vulkan), vkd3d-shader, SDL2
#   usr/share/ctrnf/setup/               tools/setup_shaders.py and the modules it uses
# Nothing of the game is in it: the first start builds the shaders from the player's dump.
# usage: make_appimage.sh <runner binary> <DXVK lib dir> <vkd3d prefix> <out dir> [version]
set -e
EXE=$1 DXVK_LIB=$2 VKD3D=$3 OUT=$4 VERSION=${5:-dev}
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$HERE/../.."
APPDIR="$OUT/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/ctrnf/setup"

cp "$EXE" "$APPDIR/usr/bin/CrashTeamRacingNitroFueled"
cp "$VKD3D/bin/vkd3d-compiler" "$APPDIR/usr/bin/"
cp -L "$DXVK_LIB"/libdxvk_d3d11.so.0 "$DXVK_LIB"/libdxvk_dxgi.so.0 "$APPDIR/usr/lib/"
cp -L "$VKD3D"/lib/libvkd3d-shader.so.1 "$APPDIR/usr/lib/"
SDL=$(ldd "$EXE" | awk '/libSDL2-2.0.so.0/ {print $3}')
[ -n "$SDL" ] && cp -L "$SDL" "$APPDIR/usr/lib/"
patchelf --set-rpath '$ORIGIN/../lib' "$APPDIR/usr/bin/CrashTeamRacingNitroFueled" "$APPDIR/usr/bin/vkd3d-compiler"
for f in "$APPDIR"/usr/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$f"; done

for f in setup_shaders.py runtime_shaders.json iga.py igz.py export_shaders.py gcn.py gcn2hlsl.py \
         pack_shaders.py font_glyphs.py make_glyph_atlas.py; do
    cp "$REPO/tools/$f" "$APPDIR/usr/share/ctrnf/setup/"
done
cp "$REPO/LICENSE" "$APPDIR/usr/share/ctrnf/LICENSE"

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/CrashTeamRacingNitroFueled" "$@"
EOF
chmod +x "$APPDIR/AppRun"
cat > "$APPDIR/ctrnf.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Crash Team Racing Nitro-Fueled
Comment=Native runner for your own PS4 dump of CTR Nitro-Fueled
Exec=CrashTeamRacingNitroFueled
Icon=ctrnf
Categories=Game;
Terminal=false
EOF
cp "$REPO/port/res/icon.png" "$APPDIR/ctrnf.png"

if [ ! -x "$OUT/appimagetool" ]; then
    curl -fsSL -o "$OUT/appimagetool" \
        https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$OUT/appimagetool"
fi
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$OUT/appimagetool" --no-appstream "$APPDIR" \
    "$OUT/CTR-NitroFueled-PC-$VERSION-x86_64.AppImage"
ls -la "$OUT"/*.AppImage
