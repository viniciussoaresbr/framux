#!/usr/bin/env bash
# Gera o pacote do Framux para Windows a partir de um build do meson:
#   dist/framux-<versão>-windows-x64/          pasta portátil (roda sem instalar)
#   dist/framux-<versão>-windows-x64.zip       a mesma pasta compactada
#   dist/framux-<versão>-windows-x64-setup.exe instalador (NSIS)
#
# uso, no shell UCRT64 do MSYS2, na raiz do repositório:
#   meson setup build-win --buildtype=release -Dtests=false
#   ninja -C build-win
#   packaging/windows/build-dist.sh build-win
set -euo pipefail

BUILD=${1:-build-win}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
VERSION=$(sed -n "s/^  version : '\(.*\)',$/\1/p" meson.build)
NAME=framux-$VERSION-windows-x64
DIST=dist/$NAME
# Prefixo do MSYS2 (de onde vêm as DLLs): o do shell UCRT64 ou, em outro
# shell (Git Bash, por exemplo), a pasta do ntldd que está no PATH.
if [ -n "${MINGW_PREFIX:-}" ]; then
  PREFIX=$(cygpath -u "$MINGW_PREFIX")
else
  PREFIX=$(cd "$(dirname "$(command -v ntldd)")/.." && pwd)
fi

rm -rf "$DIST" "dist/$NAME.zip" "dist/$NAME-setup.exe"
mkdir -p "$DIST"

cp "$BUILD/src/cli/framux.exe" "$BUILD/src/daemon/windows/framuxd.exe" "$BUILD/src/gui/framux-gui.exe" \
  "$BUILD/src/layer/framux_vk.dll" "$BUILD/src/layer/framux_vk_x86_64.json" "$DIST/"
cp assets/icons/framux.ico "$DIST/"
cp LICENSE "$DIST/LICENSE.txt"
cp packaging/windows/LEIA-ME.txt "$DIST/"

# Qt: DLLs e plugins (plataforma Windows, estilo, formatos de imagem).
windeployqt6 --no-translations --no-system-d3d-compiler --no-opengl-sw --no-network --dir "$DIST" \
  "$DIST/framux-gui.exe" >/dev/null

# DLLs do MSYS2 de que os executáveis e os plugins dependem (FFmpeg, libpng,
# libjpeg, runtime do GCC…), de forma recursiva. As do sistema ficam de fora.
win_prefix=$(cygpath -w "$PREFIX" | sed 's/\\/\\\\/g')
for _ in 1 2 3; do
  find "$DIST" -name '*.exe' -o -name '*.dll' | while read -r f; do
    ntldd -R "$f" 2>/dev/null | grep -i "$win_prefix" | sed 's/.*=> \(.*\) (0x.*/\1/' || true
  done | sort -u | while read -r dll; do
    base=$(basename "$(cygpath -u "$dll")")
    [ -e "$DIST/$base" ] || cp "$(cygpath -u "$dll")" "$DIST/"
  done
done

echo "pasta portátil: $DIST ($(du -sh "$DIST" | cut -f1))"

# Zip da pasta portátil.
python3 - "$DIST" "dist/$NAME.zip" <<'PY'
import os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
base = os.path.basename(src)
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, files in os.walk(src):
        for f in files:
            p = os.path.join(d, f)
            z.write(p, os.path.join(base, os.path.relpath(p, src)))
PY
echo "zip: dist/$NAME.zip ($(du -sh "dist/$NAME.zip" | cut -f1))"

# O makensis precisa de um TEMP no formato do Windows (o /tmp do MSYS2 falha).
win_temp=$(cmd.exe //c 'echo %TEMP%' </dev/null | tr -d '\r')
TMP="$win_temp" TEMP="$win_temp" makensis -V2 -DVERSION="$VERSION" -DSRC="$(cygpath -w "$ROOT/$DIST")" \
  -DOUT="$(cygpath -w "$ROOT/dist/$NAME-setup.exe")" packaging/windows/framux.nsi
echo "instalador: dist/$NAME-setup.exe ($(du -sh "dist/$NAME-setup.exe" | cut -f1))"
