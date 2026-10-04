#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <build-dir> <output.AppImage>" >&2
  exit 2
fi
build_dir=$(cd "$1" && pwd)
output=$(realpath -m "$2")
command -v appimagetool >/dev/null || { echo "appimagetool is required" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
DESTDIR="$work/AppDir" cmake --install "$build_dir" --prefix /usr
mkdir -p "$work/AppDir/usr/bin"
cat > "$work/AppDir/nightfire.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Nightfire
Exec=nightfire
Icon=nightfire
Categories=Game;
Terminal=false
EOF
cat > "$work/AppDir/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/nightfire" "$@"
EOF
chmod +x "$work/AppDir/AppRun"
mkdir -p "$(dirname "$output")"
APPIMAGE_EXTRACT_AND_RUN=1 appimagetool "$work/AppDir" "$output"
