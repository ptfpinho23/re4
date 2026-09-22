#!/usr/bin/env python3
"""The game's data tree for the PSP port (port/src/port_dvd.cpp).

The port reads the game's files from a directory next to the EBOOT, ms0:/PSP/GAME/RE4/data/ on
the memory stick, laid out like the disc (bgm/bio4str.hed, etc/core.das, st1/r100.das, ...), both
discs merged into one tree. This tool fills that directory from the GameCube disc images and is
where the asset converters plug in (CONVERTERS: per path pattern, a function from the file's
bytes to the port's bytes; none yet, so the tree still holds big-endian game data).

    gamedata.py extract re4_disc1.iso build/port/data      # disc 1 (main.dol and most files)
    gamedata.py extract re4_disc2.gcm build/port/data      # disc 2 on top (the island stage)
    gamedata.py list re4_disc1.iso                         # print a disc's file table
    gamedata.py selftest

Then copy build/port/EBOOT.PBP and build/port/data/ to ms0:/PSP/GAME/RE4/ (port/run-ppsspp.sh
does the same into PPSSPP's memory stick folder).
"""
import argparse
import fnmatch
import struct
import sys
import tempfile
from pathlib import Path

GAME_CODE = b"G4BE08"

# path pattern (fnmatch on the lower-case disc path) -> function(bytes) -> bytes
CONVERTERS = {}


def convert_file(path: str, data: bytes) -> bytes:
    for pat, fn in CONVERTERS.items():
        if fnmatch.fnmatch(path.lower(), pat):
            return fn(data)
    return data


def read_fst(f):
    """The disc's file table: [(path, offset, length)] for every file, big-endian as on the disc."""
    f.seek(0x424)
    fst_ofs, fst_size = struct.unpack(">II", f.read(8))
    f.seek(fst_ofs)
    raw = f.read(fst_size)
    n = struct.unpack_from(">I", raw, 8)[0]
    strings = raw[n * 12:]
    files = []
    stack = []  # (end index, name)
    for i in range(n):
        w0, w1, w2 = struct.unpack_from(">III", raw, i * 12)
        while stack and i >= stack[-1][0]:
            stack.pop()
        is_dir = w0 >> 24 != 0
        if i == 0:
            name = ""
        else:
            ofs = w0 & 0xFFFFFF
            name = strings[ofs:strings.index(b"\0", ofs)].decode("ascii", "replace")
        path = "/".join([s[1] for s in stack[1:]] + ([name] if i else []))
        if is_dir:
            stack.append((w2, name))
        elif i:
            files.append((path, w1, w2))
    return files


def extract(iso: Path, out: Path):
    with open(iso, "rb") as f:
        game = f.read(6)
        disc = f.read(1)[0] + 1
        if game != GAME_CODE:
            print(f"warning: {iso}: game code {game!r}, expected {GAME_CODE!r}")
        files = read_fst(f)
        total = 0
        for path, ofs, length in files:
            dst = out / path
            dst.parent.mkdir(parents=True, exist_ok=True)
            f.seek(ofs)
            dst.write_bytes(convert_file(path, f.read(length)))
            total += length
    print(f"{iso}: disc {disc}, {len(files)} files, {total / 1e6:.1f} MB -> {out}")


def list_disc(iso: Path):
    with open(iso, "rb") as f:
        for path, ofs, length in read_fst(f):
            print(f"{ofs:>10x} {length:>8x}  {path}")


def make_test_iso(root: Path, iso: Path, disc: int):
    """A small big-endian disc image from a directory (the layout the extractor reads)."""
    entries = [[1 << 24, 0, 0]]
    strings = bytearray()
    files = []

    def add_name(name):
        ofs = len(strings)
        strings.extend(name.encode() + b"\0")
        return ofs

    def walk(d, parent, rel):
        for p in sorted(d.iterdir(), key=lambda p: p.name.lower()):
            r = f"{rel}/{p.name}" if rel else p.name
            if p.is_dir():
                idx = len(entries)
                entries.append([(1 << 24) | add_name(p.name), parent, 0])
                walk(p, idx, r)
                entries[idx][2] = len(entries)
            else:
                entries.append([add_name(p.name), 0, p.stat().st_size])
                files.append((len(entries) - 1, p))

    walk(root, 0, "")
    entries[0][2] = len(entries)
    fst_ofs = 0x1000
    fst_size = len(entries) * 12 + len(strings)
    pos = (fst_ofs + fst_size + 0x7FFF) & ~0x7FFF
    for idx, p in files:
        entries[idx][1] = pos
        pos = (pos + entries[idx][2] + 31) & ~31
    with open(iso, "wb") as f:
        f.write(GAME_CODE + bytes([disc - 1]))
        f.seek(0x424)
        f.write(struct.pack(">III", fst_ofs, fst_size, fst_size))
        f.seek(fst_ofs)
        for e in entries:
            f.write(struct.pack(">III", *e))
        f.write(strings)
        for idx, p in files:
            f.seek(entries[idx][1])
            f.write(p.read_bytes())
        f.truncate(pos)


def selftest():
    with tempfile.TemporaryDirectory() as d:
        root = Path(d) / "tree"
        (root / "bgm").mkdir(parents=True)
        (root / "etc" / "sub").mkdir(parents=True)
        (root / "bgm" / "bio4str.hed").write_bytes(bytes(range(256)) * 3)
        (root / "etc" / "sizetbl.dat").write_bytes(b"sizetbl" * 100)
        (root / "etc" / "sub" / "deep.bin").write_bytes(b"\x55" * 40)
        (root / "top.txt").write_bytes(b"top")
        iso = Path(d) / "t.iso"
        make_test_iso(root, iso, 1)
        out = Path(d) / "out"
        extract(iso, out)
        for p in root.rglob("*"):
            if p.is_file():
                rel = p.relative_to(root)
                assert (out / rel).read_bytes() == p.read_bytes(), rel
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    x = sub.add_parser("extract"); x.add_argument("iso", type=Path); x.add_argument("out", type=Path)
    l = sub.add_parser("list"); l.add_argument("iso", type=Path)
    sub.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "extract":
        extract(a.iso, a.out)
    elif a.cmd == "list":
        list_disc(a.iso)
    else:
        selftest()


if __name__ == "__main__":
    main()
