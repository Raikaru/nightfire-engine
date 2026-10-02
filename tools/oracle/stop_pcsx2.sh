#!/bin/sh
# Close PCSX2 gracefully under KDE Wayland (no X display, so xdotool cannot see it):
# load a one-shot KWin script that calls closeWindow() on the emulator's windows.
set -e
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$(id -u)/bus}"
js="${TMPDIR:-/tmp}/closepcsx2.$$.js"
cat > "$js" <<'JS'
const ws = workspace.windowList ? workspace.windowList() : workspace.clientList();
for (const w of ws) if (/Nightfire|PCSX2/.test(w.caption)) w.closeWindow();
JS
id=$(gdbus call --session --dest org.kde.KWin --object-path /Scripting \
      --method org.kde.kwin.Scripting.loadScript "$js" closepcsx2 | grep -oE '[0-9]+')
gdbus call --session --dest org.kde.KWin --object-path "/Scripting/Script$id" \
      --method org.kde.kwin.Script.run >/dev/null
sleep 3
gdbus call --session --dest org.kde.KWin --object-path /Scripting \
      --method org.kde.kwin.Scripting.unloadScript closepcsx2 >/dev/null
rm -f "$js"
