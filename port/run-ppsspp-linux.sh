#!/bin/sh
# Installs the port into the PPSSPP (GUI) memory stick and launches it in a window.
#   port/run-ppsspp-linux.sh            build/port/EBOOT.PBP + build/port/data -> ~/.config/ppsspp/PSP/GAME/RE4/
# In the game: the memory-card prompt and the debug room menu are answered with cross (X = the
# PSP cross button in PPSSPP's default keyboard map: X key; start = space, d-pad = arrow keys).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MS="$HOME/.config/ppsspp/PSP/GAME/RE4"
mkdir -p "$MS"
cp "$ROOT/build/port/EBOOT.PBP" "$MS/EBOOT.PBP"
[ -e "$MS/data" ] || ln -s "$ROOT/build/port/data" "$MS/data"
rm -f "$MS/port.log"
echo "installed into $MS; the port's log is $MS/port.log"
exec PPSSPPSDL "$MS/EBOOT.PBP" --windowed "$@"
