#!/bin/sh
# Boot the port in PPSSPP the way a PSP would: EBOOT.PBP and the data tree installed as
# ms0:/PSP/GAME/RE4/ in PPSSPP's memory stick folder, then print what the game logged.
#   port/run-ppsspp.sh [seconds]          PPSSPP_FLAGS=-d for the emulator's debug log
# PPSSPP records a graphics backend as failed when it is killed before a clean exit, so the
# record is cleared first and the app is quit through an Apple event, not a signal.
secs=${1:-30}
here=$(cd "$(dirname "$0")" && pwd)
build=$here/../build/port
ms=~/.config/ppsspp/PSP/GAME/RE4
app=/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL
log=$build/ppsspp.log
mkdir -p "$ms"
cp "$build/EBOOT.PBP" "$ms/EBOOT.PBP"
if [ -d "$build/data" ] && [ ! -e "$ms/data" ]; then ln -s "$build/data" "$ms/data"; fi
rm -f ~/.config/ppsspp/PSP/SYSTEM/FailedGraphicsBackends.txt
"$app" $PPSSPP_FLAGS --escape-exit "$ms/EBOOT.PBP" > "$log" 2>&1 &
pid=$!
sleep "$secs"
osascript -e 'quit app "PPSSPPSDL"' > /dev/null 2>&1
sleep 3
kill "$pid" 2>/dev/null
grep -E 'Booted|\[port\]|\[dvd\]|HALT|Exception|Illegal|Unmapped|MemoryException|breakpoint|stdout|Unimplemented|E\[' "$log" \
    | grep -v 'controls.ini\|ISO looks bogus\|Exception handler not' \
    | cut -c1-220 | awk '{k=$0; sub(/^[0-9]+:[0-9:.]+ +/,"",k); if(!seen[k]++) print}'
echo "--- full log: $log ($(grep -c '' "$log") lines)"
