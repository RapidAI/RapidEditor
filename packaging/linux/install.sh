#!/usr/bin/env bash
# Adaptive Linux installer for RapidEditor.
#
#   ./install.sh                          # use an AppImage next to this script,
#                                         # else download the matching release
#   ./install.sh v1.0.0 /path/to/file.AppImage
#   ./install.sh --dir /path/to/releases
#
# It detects x86_64 / aarch64, installs the matching AppImage into
# ~/.local/bin and registers a right-click / "Open With" entry. It never
# changes the user's default application. No root required.
set -euo pipefail

TAG="latest"
SRC=""
SEARCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

while [ $# -gt 0 ]; do
    case "$1" in
        --dir)   SEARCH_DIR="$2"; shift 2 ;;
        --tag)   TAG="$2"; shift 2 ;;
        --file)  SRC="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,12p' "$0"; exit 0 ;;
        *)
            if [ -f "$1" ]; then SRC="$1"; else TAG="$1"; fi
            shift ;;
    esac
done

case "$(uname -m | tr '[:upper:]' '[:lower:]')" in
    x86_64|amd64)  ARCH="x86_64" ;;
    aarch64|arm64) ARCH="aarch64" ;;
    *) echo "ERROR: unsupported architecture $(uname -m). Only x86_64 and aarch64 are shipped." >&2; exit 1 ;;
esac
echo "Detected architecture: $ARCH"

TMPFILE=""
if [ -z "$SRC" ]; then
    # Prefer a local AppImage of the right architecture.
    for f in "$SEARCH_DIR"/RapidEditor-*-linux-"$ARCH".AppImage; do
        [ -f "$f" ] && { SRC="$f"; break; }
    done
fi

if [ -z "$SRC" ]; then
    echo "No local AppImage for $ARCH; resolving the GitHub release asset ..."
    if [ "$TAG" = "latest" ]; then
        API="https://api.github.com/repos/RapidAI/RapidEditor/releases/latest"
    else
        API="https://api.github.com/repos/RapidAI/RapidEditor/releases/tags/$TAG"
    fi
    JSON="$(curl -fsSL "$API")"
    if command -v python3 >/dev/null 2>&1; then
        URL="$(printf '%s' "$JSON" | python3 -c '
import json, sys
data = json.load(sys.stdin)
arch = sys.argv[1]
for a in data.get("assets", []):
    if a.get("name", "").endswith("-linux-" + arch + ".AppImage"):
        print(a["browser_download_url"]); break
' "$ARCH")"
    else
        URL="$(printf '%s' "$JSON" | grep -o "\"browser_download_url\": *\"[^\"]*-linux-$ARCH\.AppImage\"" \
               | head -n 1 | cut -d '"' -f 4)"
    fi
    [ -n "$URL" ] || { echo "ERROR: no linux-$ARCH AppImage in release $TAG" >&2; exit 1; }
    TMPFILE="$(mktemp)"
    echo "Downloading $URL"
    curl -fL --retry 3 -o "$TMPFILE" "$URL"
    SRC="$TMPFILE"
fi

# Guard against installing the wrong architecture from an explicit --file.
if command -v file >/dev/null 2>&1 && ! file -L "$SRC" | grep -qi 'ELF'; then
    echo "WARNING: $SRC may not be an AppImage (file(1) did not report ELF)." >&2
fi

BIN_DIR="${XDG_BIN_HOME:-$HOME/.local/bin}"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}"
ICON_DIR="$DATA_DIR/icons/hicolor/256x256/apps"
APP_DIR="$DATA_DIR/applications"
ACTION_DIR="$DATA_DIR/file-manager/actions"
mkdir -p "$BIN_DIR" "$ICON_DIR" "$APP_DIR" "$ACTION_DIR"

TARGET="$BIN_DIR/RapidEditor.AppImage"
cp "$SRC" "$TARGET"
chmod 755 "$TARGET"
[ -z "$TMPFILE" ] || rm -f "$TMPFILE"
echo "Installed: $TARGET"

ICON_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../assets/rapideditor.png"
ICON="$ICON_DIR/rapideditor.png"
if [ -f "$ICON_SRC" ]; then
    cp "$ICON_SRC" "$ICON"
else
    # Standalone download: take the icon from the GitHub repository.
    curl -fsSL -o "$ICON" \
      "https://raw.githubusercontent.com/RapidAI/RapidEditor/main/assets/rapideditor.png" || true
fi

DESKTOP="$APP_DIR/com.rapidaire.rapideditor.desktop"
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=RapidEditor
GenericName=Fast text and code editor
GenericName[zh_CN]=快速文本与源码编辑器
Comment=Multi-tab editor with syntax highlighting and large-file support
Exec=$TARGET %F
Icon=$ICON
Terminal=false
Categories=Utility;TextEditor;Development;
StartupNotify=true
StartupWMClass=RapidEditor
MimeType=text/plain;text/x-csrc;text/x-chdr;text/x-c++src;text/x-python;application/json;application/xml;application/x-shellscript;text/markdown;text/x-log;text/csv;text/x-sql;
EOF
chmod 644 "$DESKTOP"

# Right-click action for file managers that support file-manager/actions
# (Nemo, Caja, Nautilus + filemanager-actions). GNOME/KDE also expose the
# MimeType association above as "Open With > RapidEditor".
ACTION="$ACTION_DIR/rapideditor-open.desktop"
cat > "$ACTION" <<EOF
[Desktop Entry]
Type=Action
Name=Open with RapidEditor
Name[zh_CN]=用 RapidEditor 打开
Icon=$ICON
Profiles=profile-zero;

[X-Action-Profile profile-zero]
Exec=$TARGET %F
MimeTypes=all/allfiles;
EOF
chmod 644 "$ACTION"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$APP_DIR" 2>/dev/null || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$DATA_DIR/icons/hicolor" 2>/dev/null || true
fi

cat <<EOF

RapidEditor ($ARCH) installed.
  Binary : $TARGET
  Menu   : $DESKTOP
  Action : $ACTION

Right-click a file -> "Open With" -> RapidEditor (or the file manager's
"用 RapidEditor 打开" action). Restart the file manager if the entry is not
visible yet. Uninstall with packaging/linux/uninstall.sh.
EOF
