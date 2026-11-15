#!/usr/bin/env bash
# Build a RapidEditor AppImage for the host architecture (x86_64 / aarch64).
#
#   packaging/linux/build_appimage.sh [version]
#
# Architecture is detected with scripts/arch.sh (uname -m). An AppImage can
# only carry one ELF architecture, so CI builds one per runner; install.sh
# picks the right one at install time. Override with RE_ARCH=... for a cross
# build where the toolchain is already set up.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# shellcheck source=../../scripts/arch.sh
source "$ROOT/scripts/arch.sh"

VERSION="${1:-1.0.0}"
ARCH="$(re_arch)"

if [ "$(uname -s)" != "Linux" ]; then
    echo "ERROR: AppImage can only be built on Linux (use GitHub Actions ubuntu-* runners)." >&2
    exit 1
fi

BUILD="$ROOT/build-linux-$ARCH"
echo "==> Building RapidEditor for $ARCH ..."
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --parallel

EXE="$BUILD/RapidEditor"
[ -f "$EXE" ] || { echo "ERROR: $EXE not found" >&2; exit 1; }

DIST="$ROOT/dist"
APPDIR="$DIST/AppDir-$ARCH"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp "$EXE" "$APPDIR/usr/bin/RapidEditor"
chmod 755 "$APPDIR/usr/bin/RapidEditor"

echo "==> Bundling runtime libraries ..."
python3 "$HERE/bundle_libs.py" "$EXE" "$APPDIR/usr/lib"

# AppImage icon: root-level .png + .DirIcon are what appimagetool reads.
cp "$ROOT/assets/rapideditor.png" "$APPDIR/rapideditor.png"
cp "$ROOT/assets/rapideditor.png" "$APPDIR/.DirIcon"
cp "$ROOT/assets/rapideditor.png" \
   "$APPDIR/usr/share/icons/hicolor/256x256/apps/rapideditor.png"

sed -e 's|__EXEC__|RapidEditor %F|' -e 's|__ICON__|rapideditor|' \
    "$HERE/rapideditor.desktop.in" > "$APPDIR/rapideditor.desktop"
cp "$APPDIR/rapideditor.desktop" "$APPDIR/usr/share/applications/rapideditor.desktop"
cp "$HERE/AppRun.in" "$APPDIR/AppRun"
chmod 755 "$APPDIR/AppRun"

# appimagetool itself is distributed as an AppImage; --appimage-extract-and-run
# avoids needing libfuse2 on Ubuntu 24.04 runners.
TOOLS="$DIST/tools"
mkdir -p "$TOOLS"
if [ -n "${APPIMAGETOOL:-}" ]; then
    TOOL="$APPIMAGETOOL"
    RUN=(env "ARCH=$ARCH" "$TOOL")
else
    TOOL="$TOOLS/appimagetool-$ARCH.AppImage"
    if [ ! -f "$TOOL" ]; then
        echo "==> Downloading appimagetool ($ARCH) ..."
        curl -fL --retry 3 \
             -o "$TOOL" \
             "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
        chmod 755 "$TOOL"
    fi
    RUN=(env "ARCH=$ARCH" "$TOOL" --appimage-extract-and-run)
fi

OUT="$DIST/RapidEditor-$VERSION-linux-$ARCH.AppImage"
echo "==> Creating $OUT ..."
"${RUN[@]}" "$APPDIR" "$OUT"
chmod 755 "$OUT"
echo "Done: $OUT"
