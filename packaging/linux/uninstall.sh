#!/usr/bin/env bash
# Remove the user-level integration installed by install.sh.
set -euo pipefail

DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}"
BIN_DIR="${XDG_BIN_HOME:-$HOME/.local/bin}"

rm -f "$BIN_DIR/RapidEditor.AppImage" \
      "$DATA_DIR/applications/com.rapidaire.rapideditor.desktop" \
      "$DATA_DIR/file-manager/actions/rapideditor-open.desktop" \
      "$DATA_DIR/icons/hicolor/256x256/apps/rapideditor.png"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$DATA_DIR/applications" 2>/dev/null || true
fi
echo "RapidEditor integration removed."
