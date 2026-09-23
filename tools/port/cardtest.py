#!/usr/bin/env python3
"""Host test of the memory card layer (port/src/port_card.cpp): builds port/test/card_host with the
PSP file calls mapped to POSIX and checks a create / write / read / status / delete round trip
in a temporary directory.    python3 tools/port/cardtest.py"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "build" / "port" / "card_host"

cmd = ["clang++", "-std=c++14", "-O1", "-w", "-DRE4_PORT=1", "-I", str(ROOT / "port/test/hostinc"), "-I", str(ROOT / "port/include"),
       "-I", str(ROOT / "include"), str(ROOT / "port/src/port_card.cpp"), str(ROOT / "port/test/card_host.cpp"), "-o", str(HOST)]
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode != 0:
    sys.exit("harness build failed:\n" + r.stderr)
with tempfile.TemporaryDirectory() as tmp:
    (Path(tmp) / "data").mkdir()
    r = subprocess.run([str(HOST), tmp], capture_output=True, text=True)
    got = r.stdout.strip().split("\n")
    files = sorted(p.name for p in (Path(tmp) / "card").iterdir()) if (Path(tmp) / "card").exists() else None
expected = ["probe 0 16 8192", "mount 0", "free 0 2015232 127", "open-missing -4", "create 0 no 0 len 24576", "write 0", "read 0 same 1",
            "read0 0 zero 1", "stat 0 len 24576 comment ffffffff name bh4_data00", "setstat 0", "stat2 0 comment 40 icon 80 time 123456",
            "close 0", "reopen 0 len 24576", "create-exists -7", "delete 0", "open-deleted -4"]
for g, e in zip(got, expected):
    if g != e:
        sys.exit(f"FAIL: got {g!r}, expected {e!r}\n" + "\n".join(got))
if len(got) != len(expected):
    sys.exit("FAIL: line count\n" + "\n".join(got))
if files != []:
    sys.exit(f"FAIL: files left in the card directory: {files}")
print("cardtest ok")
