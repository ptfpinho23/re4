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
    gamedata.py synthesize build/port/data                 # the files the retail discs lack (etc/moji8.tpl)
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
    synthesize(out)


# --- files the retail discs do not have -------------------------------------------------------
# The debug build's on-screen text (game/eprintf.cpp: the debug menus, the log) needs etc/moji8.tpl,
# an 8 x 16 font sheet the retail discs never shipped: 32 glyphs per row from 0x20, 8 rows, in a
# 256-wide I8 texture. `synthesize` builds one from a Linux console font (PSF).

PSF_FONTS = ["default8x16.psfu.gz", "lat1-16.psfu.gz", "lat9w-16.psfu.gz", "cp850-8x16.psfu.gz"]
PSF_DIRS = ["/usr/share/kbd/consolefonts", "/usr/share/consolefonts", "/usr/lib/kbd/consolefonts"]


def find_psf():
    for d in PSF_DIRS:
        for n in PSF_FONTS:
            p = Path(d) / n
            if p.exists():
                return p
    return None


def read_psf(path: Path):
    """The glyph bitmaps of an 8 x 16 PSF1 / PSF2 console font: {codepoint: 16 row bytes}."""
    import gzip
    blob = gzip.open(path).read() if str(path).endswith(".gz") else path.read_bytes()
    if blob[:2] == b"\x36\x04":  # PSF1
        mode, charsize = blob[2], blob[3]
        n = 512 if mode & 1 else 256
        glyphs = [blob[4 + i * charsize:4 + (i + 1) * charsize] for i in range(n)]
        table = blob[4 + n * charsize:] if mode & 2 else None
        height, width, unicode16 = charsize, 8, True
    elif blob[:4] == b"\x72\xb5\x4a\x86":  # PSF2
        version, hdr, flags, n, bpg, height, width = struct.unpack_from("<7I", blob, 4)
        glyphs = [blob[hdr + i * bpg:hdr + (i + 1) * bpg] for i in range(n)]
        table = blob[hdr + n * bpg:] if flags & 1 else None
        unicode16 = False
    else:
        raise ValueError(f"{path}: not a PSF font")
    if width != 8 or height != 16:
        raise ValueError(f"{path}: {width}x{height}, need 8x16")
    out = {}
    if table is None:
        for i, g in enumerate(glyphs):
            out[i] = g
        return out
    # the unicode table: per glyph, a list of code points, terminated by 0xFFFF (PSF1) / 0xFF (PSF2)
    i = 0
    pos = 0
    while pos < len(table) and i < len(glyphs):
        if unicode16:
            cps = []
            while pos + 1 < len(table):
                cp = struct.unpack_from("<H", table, pos)[0]
                pos += 2
                if cp == 0xFFFF:
                    break
                if cp != 0xFFFE:
                    cps.append(cp)
        else:
            end = table.index(b"\xff", pos)
            seq = table[pos:end]
            pos = end + 1
            cps = []
            for part in seq.split(b"\xfe"):
                try:
                    cps.extend(ord(c) for c in part.decode("utf-8"))
                except UnicodeDecodeError:
                    pass
        for cp in cps:
            out.setdefault(cp, glyphs[i])
        i += 1
    return out


def make_moji8_tpl(psf: Path) -> bytes:
    """etc/moji8.tpl: a one-texture TPL (256 x 128 I8, GameCube tiling) of glyphs 0x20..0x7F and
    0x80..0xFF from the console font, laid out the way eprintf.cpp's font_draw reads them."""
    glyphs = read_psf(psf)
    W, H = 256, 128
    img = bytearray(W * H)
    for code in range(0x20, 0x100):
        g = glyphs.get(code) or glyphs.get(0x3F, b"\0" * 16)  # '?' for anything the font lacks
        c = code - 0x20
        x0, y0 = (c & 0x1F) * 8, ((c >> 5) & 7) * 16
        for y in range(16):
            row = g[y] if y < len(g) else 0
            for x in range(8):
                if row & (0x80 >> x):
                    img[(y0 + y) * W + x0 + x] = 0xFF
    # I8 tiles: 8 x 4 texels, 32 bytes each, in row-major tile order
    tiled = bytearray()
    for ty in range(H // 4):
        for tx in range(W // 8):
            for y in range(4):
                base = (ty * 4 + y) * W + tx * 8
                tiled += img[base:base + 8]
    data_ofs = 0x40
    hdr = struct.pack(">III", 0x0020AF30, 1, 0x0C)                     # TEXPalette
    hdr += struct.pack(">II", 0x14, 0)                                # TEXDescriptor: texture, no CLUT
    hdr += struct.pack(">HHIIIIIIfBBBB", H, W, 1, data_ofs, 0, 0, 1, 1, 0.0, 0, 0, 0, 0)  # TEXHeader: I8, clamp, linear
    hdr += b"\0" * (data_ofs - len(hdr))
    return bytes(hdr) + bytes(tiled)


def make_room_info(out: Path) -> bytes:
    """debug/roomInfo.dat: the debug build's room-jump table (game/room_jmp.cpp cRoomJmp), which the
    title's debug menu and the room-jump menu read: u32 stage count, u32 offsets per stage (0 = none),
    per stage u32 count + CRoomInfo records (0x20 bytes: flag, stage, room, position, angle, three
    string offsets), then the strings. One jump point per room found under stN/, no start position."""
    stages = {}
    for d in sorted(out.glob("[sS]t[0-9]")):
        st = int(d.name[2])
        rooms = sorted({int(f.stem[1:4], 16) for f in d.iterdir() if f.suffix == ".das" and f.stem[0] == "r" and len(f.stem) == 4})
        if rooms:
            stages[st] = rooms
    n_stages = max(stages) + 1 if stages else 1
    strings = bytearray()
    str_ofs = {}

    def string(txt):
        if txt not in str_ofs:
            str_ofs[txt] = len(strings)
            strings.extend(txt.encode() + b"\0")
        return str_ofs[txt]

    tables = bytearray()
    ofs = []
    head_size = 4 + 4 * n_stages
    records = []  # (table position, list of (roomNo, name, person, person2))
    for st in range(n_stages):
        if st not in stages:
            ofs.append(0)
            continue
        ofs.append(head_size + len(tables))
        tables += struct.pack(">I", len(stages[st]))
        for room in stages[st]:
            records.append((len(tables), room, "r%03x" % room, "-", "-"))
            tables += b"\0" * 0x20
    str_base = head_size + len(tables)
    for pos, room, name, person, person2 in records:
        struct.pack_into(">HHfffdIII" if False else ">HHffffIII", tables, pos, 0, room, 0.0, 0.0, 0.0, 0.0,
                         str_base + string(name), str_base + string(person), str_base + string(person2))
    head = struct.pack(">I", n_stages) + b"".join(struct.pack(">I", o) for o in ofs)
    blob = head + bytes(tables) + bytes(strings)
    return blob + b"\0" * (-len(blob) % 32)


def synthesize(out: Path):
    """Writes the files the port needs that the retail discs lack (skips the ones already there)."""
    info = out / "debug" / "roomInfo.dat"
    if not info.exists():
        info.parent.mkdir(parents=True, exist_ok=True)
        info.write_bytes(make_room_info(out))
        print(f"synthesized {info}")
    tpl = out / "etc" / "moji8.tpl"
    if not tpl.exists():
        psf = find_psf()
        if psf is None:
            print("note: no 8x16 PSF console font found (kbd package): etc/moji8.tpl not synthesized, the debug text stays off")
        else:
            tpl.parent.mkdir(parents=True, exist_ok=True)
            tpl.write_bytes(make_moji8_tpl(psf))
            print(f"synthesized {tpl} from {psf}")


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
    y = sub.add_parser("synthesize"); y.add_argument("out", type=Path)
    a = ap.parse_args()
    if a.cmd == "extract":
        extract(a.iso, a.out)
    elif a.cmd == "list":
        list_disc(a.iso)
    elif a.cmd == "synthesize":
        synthesize(a.out)
    else:
        selftest()


if __name__ == "__main__":
    main()
