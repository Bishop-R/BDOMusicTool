#!/bin/bash
# Install Composer desktop entry and icon for the current user
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ICON_SRC="$SCRIPT_DIR/src/Icons/TrebleClef.png"
DESKTOP_SRC="$SCRIPT_DIR/src/composer.desktop"

mkdir -p ~/.local/share/icons/hicolor/256x256/apps
mkdir -p ~/.local/share/applications

cp "$ICON_SRC" ~/.local/share/icons/hicolor/256x256/apps/composer.png
cp "$DESKTOP_SRC" ~/.local/share/applications/composer.desktop

# Update icon cache if possible
gtk-update-icon-cache -f -t ~/.local/share/icons/hicolor/ 2>/dev/null || true

echo "Installed. You may need to log out and back in for the icon to appear."
