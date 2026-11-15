#!/usr/bin/env bash
# Build a RapidEditor .pkg for macOS.
#
#   packaging/macos/build_pkg.sh [version] [universal|auto|x86_64|arm64]
#
# "universal" (the default) builds one binary containing both x86_64 and arm64
# slices, so a single .pkg installs correctly on Intel and Apple Silicon Macs.
# "auto" builds just the host architecture (auto-detected by scripts/arch.sh).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# shellcheck source=../../scripts/arch.sh
source "$ROOT/scripts/arch.sh"

VERSION="${1:-1.0.0}"
MODE="${2:-universal}"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "ERROR: .pkg can only be built on macOS (use GitHub Actions macos-* runners)." >&2
    exit 1
fi

case "$MODE" in
    universal) OARCH="x86_64;arm64"; TAG="universal" ;;
    auto)       OARCH="$(re_macos_arch)"; TAG="$(re_short)" ;;
    x86_64)     OARCH="x86_64"; TAG="x64" ;;
    arm64)      OARCH="arm64";  TAG="arm64" ;;
    *) echo "ERROR: unknown mode '$MODE' (use universal|auto|x86_64|arm64)" >&2; exit 2 ;;
esac

BUILD="$ROOT/build-macos-$TAG"
echo "==> Building RapidEditor ($OARCH) ..."
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="$OARCH"
cmake --build "$BUILD" --parallel

EXE="$BUILD/RapidEditor"
[ -f "$EXE" ] || { echo "ERROR: $EXE not found" >&2; exit 1; }
echo "==> Binary architectures: $(lipo -archs "$EXE" 2>/dev/null || echo unknown)"

STAGE="$(mktemp -d)/root"
APP="$STAGE/Applications/RapidEditor.app/Contents"
SERVICES="$STAGE/Library/Services"
mkdir -p "$APP/MacOS" "$APP/Resources" "$SERVICES" "$ROOT/dist"

cp "$EXE" "$APP/MacOS/RapidEditor"
chmod 755 "$APP/MacOS/RapidEditor"
cp "$ROOT/assets/rapideditor.icns" "$APP/Resources/rapideditor.icns"
sed "s/__VERSION__/$VERSION/g" "$HERE/Info.plist.in" > "$APP/Info.plist"
/usr/bin/plutil -lint "$APP/Info.plist" >/dev/null

echo "==> Creating Finder Quick Action (right-click menu) ..."
python3 "$HERE/make_quick_action.py" \
        "$SERVICES/Open with RapidEditor.workflow" "$VERSION"
/usr/bin/plutil -lint "$SERVICES/Open with RapidEditor.workflow/Contents/document.wflow" >/dev/null

# Git on Windows does not preserve the executable bit reliably; pkgbuild needs
# postinstall to be executable, so enforce it here as well as in the repo.
chmod 755 "$HERE/scripts/postinstall" "$HERE/scripts/enable_service.py"

OUT="$ROOT/dist/RapidEditor-$VERSION-macos-$TAG.pkg"
echo "==> Packaging $OUT ..."
pkgbuild --root "$STAGE" \
         --identifier "com.rapidaire.rapideditor" \
         --version "$VERSION" \
         --install-location "/" \
         --scripts "$HERE/scripts" \
         "$OUT"

echo "Done: $OUT"
