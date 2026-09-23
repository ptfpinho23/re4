#!/bin/sh
# Boot the port in PPSSPPHeadless (Linux): installs EBOOT.PBP and the data tree as
# ms0:/PSP/GAME/RE4/ in the headless memory stick (~/.ppsspp), runs it for N seconds with the
# interpreter (guest faults are reported with the PC) and prints the game's log, de-duplicated.
#   port/run-headless.sh [seconds] [extra PPSSPPHeadless flags...]     RAW=1 to print every line
secs=${1:-30}; [ $# -gt 0 ] && shift
here=$(cd "$(dirname "$0")" && pwd)
build=$here/../build/port
ms=~/.ppsspp/PSP/GAME/RE4
log=$build/headless.log
mkdir -p "$ms"
cp "$build/EBOOT.PBP" "$ms/EBOOT.PBP"
if [ -d "$build/data" ] && [ ! -e "$ms/data" ]; then ln -s "$(cd "$build" && pwd)/data" "$ms/data"; fi
timeout $((secs + 60)) PPSSPPHeadless "$ms/EBOOT.PBP" --timeout="$secs" -i -l "$@" > "$log" 2>&1
if [ -n "$RAW" ]; then cat "$log"; exit 0; fi
grep -E 'stdout|\[port\]|\[dvd\]|HALT|Exception|Illegal|Unmapped|MemoryException|breakpoint|Unimplemented|^E |^N |Crash|crash' "$log" \
    | grep -v 'ISO looks bogus\|lang/.ini' \
    | sed -E 's/^I stdout: //' | cut -c1-200 | awk '{k=$0; sub(/^[0-9]+:[0-9:.]+ +/,"",k); if(!seen[k]++) print}'
echo "--- full log: $log ($(grep -c '' "$log") lines)"
