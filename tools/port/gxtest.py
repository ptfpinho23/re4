#!/usr/bin/env python3
"""Host test of the port's GX layer (port/src/port_gx.cpp) against an independent reference.

Builds port/test/gx_host (the GX layer with a recorder in place of the GE), then feeds it
scripts and checks what reaches the "GE":
  - every GameCube texture format, tiled here from the format descriptions (I4, I8, IA4, IA8,
    RGB565, RGB5A3, RGBA8, C4, C8, C14X2 with the three palette formats, CMPR), decoded by the
    port into 32-bit or, for CMPR, re-blocked into the GE's DXT1, compared pixel by pixel;
  - a display list with indexed big-endian attributes and per-vertex matrix indices, a quad
    list with direct data, immediate-mode writes, a texture matrix, the projection z fix.

    python3 tools/port/gxtest.py
"""
import random
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "build" / "port" / "gx_host"


def build():
    HOST.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["clang++", "-std=c++14", "-O1", "-w", "-DRE4_PORT=1", "-I", str(ROOT / "port/test/hostinc"), "-I", str(ROOT / "port/include"),
           "-I", str(ROOT / "include"), str(ROOT / "port/src/port_gx.cpp"), str(ROOT / "port/test/gx_fake_gu.cpp"),
           str(ROOT / "port/test/gx_host.cpp"), "-o", str(HOST)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("harness build failed:\n" + r.stderr)


def run(script):
    r = subprocess.run([str(HOST)], input=script, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("harness failed:\n" + r.stderr + r.stdout)
    return r.stdout.split("\n")


# ---- expansions (the GE's 8888 is bytes r, g, b, a)
def e5(v): return (v << 3) | (v >> 2)
def e6(v): return (v << 2) | (v >> 4)
def e4(v): return v * 17
def e3(v): return (v << 5) | (v << 2) | (v >> 1)


def rgb565(v):
    return (e5(v >> 11), e6((v >> 5) & 63), e5(v & 31), 255)


def rgb5a3(v):
    if v & 0x8000:
        return (e5((v >> 10) & 31), e5((v >> 5) & 31), e5(v & 31), 255)
    return (e4((v >> 8) & 15), e4((v >> 4) & 15), e4(v & 15), e3((v >> 12) & 7))


def ia8(v):
    return (v & 0xFF, v & 0xFF, v & 0xFF, v >> 8)


# ---- GameCube tiling: tile (tw, th), texels row-major inside a tile, tiles row-major
def tiled(w, h, tw, th, texel_bytes):
    """Yields (x, y) in the order the texels are stored."""
    for ty in range(h // th):
        for tx in range(w // tw):
            for y in range(th):
                for x in range(tw):
                    yield tx * tw + x, ty * th + y


def make_texture(fmt, w, h, rnd, tlut_fmt=None):
    """(gc bytes, expected pixels [(r,g,b,a)] row-major, tlut bytes or None, tlut entries)"""
    px = {}
    data = bytearray()
    tlut = None
    n_entries = 0
    if fmt == 0:  # I4
        order = list(tiled(w, h, 8, 8, 0.5))
        for i in range(0, len(order), 2):
            a, b = rnd.randrange(16), rnd.randrange(16)
            data.append(a << 4 | b)
            px[order[i]] = (e4(a),) * 3 + (255,)
            px[order[i + 1]] = (e4(b),) * 3 + (255,)
    elif fmt == 1:  # I8
        for xy in tiled(w, h, 8, 4, 1):
            v = rnd.randrange(256)
            data.append(v)
            px[xy] = (v, v, v, 255)
    elif fmt == 2:  # IA4
        for xy in tiled(w, h, 8, 4, 1):
            a, l = rnd.randrange(16), rnd.randrange(16)
            data.append(a << 4 | l)
            px[xy] = (e4(l),) * 3 + (e4(a),)
    elif fmt == 3:  # IA8
        for xy in tiled(w, h, 4, 4, 2):
            a, l = rnd.randrange(256), rnd.randrange(256)
            data += bytes([a, l])
            px[xy] = (l, l, l, a)
    elif fmt == 4:  # RGB565
        for xy in tiled(w, h, 4, 4, 2):
            v = rnd.randrange(65536)
            data += struct.pack(">H", v)
            px[xy] = rgb565(v)
    elif fmt == 5:  # RGB5A3
        for xy in tiled(w, h, 4, 4, 2):
            v = rnd.randrange(65536)
            data += struct.pack(">H", v)
            px[xy] = rgb5a3(v)
    elif fmt == 6:  # RGBA8: per 4x4 tile, 32 bytes of A,R pairs then 32 bytes of G,B pairs
        order = list(tiled(w, h, 4, 4, 4))
        for t in range(0, len(order), 16):
            texels = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(16)]
            for r, g, b, a in texels:
                data += bytes([a, r])
            for r, g, b, a in texels:
                data += bytes([g, b])
            for k in range(16):
                px[order[t + k]] = texels[k]
    elif fmt in (8, 9, 10):  # C4 / C8 / C14X2 with a palette
        n_entries = {8: 16, 9: 256, 10: 64}[fmt]
        lut_vals = [rnd.randrange(65536) for _ in range(n_entries)]
        tlut = b"".join(struct.pack(">H", v) for v in lut_vals)
        conv = {0: ia8, 1: rgb565, 2: rgb5a3}[tlut_fmt]
        if fmt == 8:
            order = list(tiled(w, h, 8, 8, 0.5))
            for i in range(0, len(order), 2):
                a, b = rnd.randrange(16), rnd.randrange(16)
                data.append(a << 4 | b)
                px[order[i]] = conv(lut_vals[a])
                px[order[i + 1]] = conv(lut_vals[b])
        elif fmt == 9:
            for xy in tiled(w, h, 8, 4, 1):
                v = rnd.randrange(256)
                data.append(v)
                px[xy] = conv(lut_vals[v])
        else:
            for xy in tiled(w, h, 4, 4, 2):
                v = rnd.randrange(n_entries)
                data += struct.pack(">H", v | (rnd.randrange(4) << 14))  # the top two bits are ignored
                px[xy] = conv(lut_vals[v])
    else:
        raise ValueError(fmt)
    pixels = [px[(x, y)] for y in range(h) for x in range(w)]
    return bytes(data), pixels, tlut, n_entries


# ---- DXT1 (CMPR) as blocks: the GC stores 8x8 tiles of four 4x4 blocks, colours big-endian,
# index bytes with texel 0 in the top bits; the GE stores blocks row-major over the texture,
# the four index bytes first (texel 0 in the low bits), then the two colours little-endian.
def dxt_decode(c0, c1, idx_rows):
    """idx_rows: 4 rows of 4 two-bit indices (row-major). Returns 16 (r,g,b,a)."""
    p0 = rgb565(c0)[:3]
    p1 = rgb565(c1)[:3]
    if c0 > c1:
        p2 = tuple((2 * a + b) // 3 for a, b in zip(p0, p1))
        p3 = tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))
        pal = [p0 + (255,), p1 + (255,), p2 + (255,), p3 + (255,)]
    else:
        p2 = tuple((a + b) // 2 for a, b in zip(p0, p1))
        pal = [p0 + (255,), p1 + (255,), p2 + (255,), (0, 0, 0, 0)]
    return [pal[i] for row in idx_rows for i in row]


def make_cmpr(w, h, rnd):
    """(gc bytes, expected 16-texel decodes per 4x4 block in row-major block order)"""
    bw, bh = w // 4, h // 4
    blocks = {}
    data = bytearray()
    for ty in range(h // 8):
        for tx in range(w // 8):
            for sub in range(4):
                bx, by = tx * 2 + (sub & 1), ty * 2 + (sub >> 1)
                c0, c1 = rnd.randrange(65536), rnd.randrange(65536)
                rows = [[rnd.randrange(4) for _ in range(4)] for _ in range(4)]
                data += struct.pack(">HH", c0, c1)
                for row in rows:
                    data.append((row[0] << 6) | (row[1] << 4) | (row[2] << 2) | row[3])
                blocks[(bx, by)] = dxt_decode(c0, c1, rows)
    expected = [blocks[(bx, by)] for by in range(bh) for bx in range(bw)]
    return bytes(data), expected


def psp_dxt1_decode(block8):
    """A GE DXT1 block: 4 index bytes (texel 0 in the low bits), colour 1, colour 2 (LE)."""
    rows = [[(block8[i] >> (2 * k)) & 3 for k in range(4)] for i in range(4)]
    c0, c1 = struct.unpack("<HH", block8[4:8])
    return dxt_decode(c0, c1, rows)


# ---- running the harness
def parse_draws(lines):
    draws = []
    i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith("DRAW "):
            _, prim, count = l.split()
            verts = []
            i += 1
            for _ in range(int(count)):
                p = lines[i].split()
                verts.append((float(p[1]), float(p[2]), int(p[3], 16), float(p[4]), float(p[5]), float(p[6])))
                i += 1
            tex = None
            if lines[i].startswith("TEX "):
                p = lines[i].split()
                tex = (int(p[1]), int(p[2]), int(p[3]), bytes.fromhex(p[4]))
            draws.append((int(prim), verts, tex))
        i += 1
    return draws


def check(cond, msg):
    if not cond:
        sys.exit("FAIL: " + msg)


def point_setup():
    # a one-vertex draw with a direct f32 position, used to trigger a texture upload
    return "clearvcd\nvcd 9 1\nvat 0 9 1 4 0\nnumtexgens 1\ntevorder 0 0 0 4\n"


def test_textures(rnd):
    for fmt, name in [(0, "I4"), (1, "I8"), (2, "IA4"), (3, "IA8"), (4, "RGB565"), (5, "RGB5A3"), (6, "RGBA8")]:
        for (w, h) in [(8, 8), (16, 8)]:
            data, pixels, _, _ = make_texture(fmt, w, h, rnd)
            script = point_setup() + f"texobj {fmt} {w} {h} {data.hex()}\nbegin 184 0 1\nputf 1\nputf 2\nputf 3\n"
            draws = parse_draws(run(script))
            check(len(draws) == 1 and draws[0][2] is not None, f"{name}: no textured draw")
            psm, tw, th, out = draws[0][2]
            check(psm == 3 and (tw, th) == (w, h), f"{name}: psm/size {psm} {tw}x{th}")
            got = [struct.unpack_from("<BBBB", out, 4 * k) for k in range(w * h)]
            bad = [k for k in range(min(len(got), len(pixels))) if got[k] != pixels[k]]
            check(got == pixels, f"{name} {w}x{h}: {len(got)} texels vs {len(pixels)}, first bad {bad[:1]}: {got[bad[0]] if bad else None} vs {pixels[bad[0]] if bad else None}")
            print(f"  {name:7s} {w}x{h} ok")
    for fmt, name in [(8, "C4"), (9, "C8"), (10, "C14X2")]:
        for tlut_fmt in (0, 1, 2):
            data, pixels, tlut, n = make_texture(fmt, 16, 8, rnd, tlut_fmt)
            script = point_setup() + f"tlut 2 {tlut_fmt} {n} {tlut.hex()}\ntexobjci {fmt} 16 8 2 {data.hex()}\nbegin 184 0 1\nputf 1\nputf 2\nputf 3\n"
            draws = parse_draws(run(script))
            psm, tw, th, out = draws[0][2]
            got = [struct.unpack_from("<BBBB", out, 4 * k) for k in range(16 * 8)]
            check(got == pixels, f"{name} tlut {tlut_fmt}: pixels differ")
            print(f"  {name:7s} tlut {tlut_fmt} ok")
    for (w, h) in [(8, 8), (16, 16), (32, 8)]:
        data, expected = make_cmpr(w, h, rnd)
        script = point_setup() + f"texobj 14 {w} {h} {data.hex()}\nbegin 184 0 1\nputf 1\nputf 2\nputf 3\n"
        draws = parse_draws(run(script))
        psm, tw, th, out = draws[0][2]
        check(psm == 8, "CMPR: not DXT1")
        nb = (w // 4) * (h // 4)
        got = [psp_dxt1_decode(out[8 * k:8 * k + 8]) for k in range(nb)]
        bad = [k for k in range(nb) if got[k] != expected[k]]
        check(got == expected, f"CMPR {w}x{h}: block decode differs at blocks {bad[:4]}")
        print(f"  CMPR    {w}x{h} ok")


def be_s16(v): return struct.pack(">h", v)


def test_display_list(rnd):
    pos = [(rnd.randrange(-2000, 2000), rnd.randrange(-2000, 2000), rnd.randrange(-2000, 2000)) for _ in range(6)]
    nrm = [(rnd.randrange(-128, 128), rnd.randrange(-128, 128), rnd.randrange(-128, 128)) for _ in range(6)]
    clr = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(6)]
    tex = [(rnd.randrange(-30000, 30000), rnd.randrange(-30000, 30000)) for _ in range(6)]
    pos_bytes = b"".join(be_s16(x) + be_s16(y) + be_s16(z) for x, y, z in pos)
    nrm_bytes = b"".join(struct.pack(">bbb", *n) for n in nrm)
    clr_bytes = b"".join(bytes(c) for c in clr)
    tex_bytes = b"".join(be_s16(s) + be_s16(t) for s, t in tex)
    m0 = [1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30]
    m3 = [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0]
    vtx = [(0, 5), (3, 2), (0, 4), (3, 1)]  # (matrix index, array index)
    dl = bytearray([0x98]) + struct.pack(">H", len(vtx))
    for mi, ai in vtx:
        dl += bytes([mi]) + struct.pack(">H", ai) + bytes([ai]) + struct.pack(">H", ai) + struct.pack(">H", ai)
    script = ("clearvcd\nvcd 0 1\nvcd 9 3\nvcd 10 2\nvcd 11 3\nvcd 13 3\n"
              "vat 0 9 1 3 4\nvat 0 10 0 1 0\nvat 0 11 1 5 0\nvat 0 13 1 3 8\n"
              f"array 9 6 {pos_bytes.hex()}\narray 10 3 {nrm_bytes.hex()}\narray 11 4 {clr_bytes.hex()}\narray 13 4 {tex_bytes.hex()}\n"
              f"posmtx 0 {' '.join(map(str, m0))}\nposmtx 3 {' '.join(map(str, m3))}\ncurmtx 0\nmatsrc 1\nnumtexgens 1\ntexgen 60\n"
              f"dl {dl.hex()}\n")
    draws = parse_draws(run(script))
    check(len(draws) == 1, "DL: one draw expected")
    prim, verts, _ = draws[0]
    check(prim == 4 and len(verts) == 4, f"DL: prim {prim} count {len(verts)}")
    for k, (mi, ai) in enumerate(vtx):
        m = m0 if mi == 0 else m3
        x, y, z = [c / 16.0 for c in pos[ai]]
        ex = (m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7], m[8] * x + m[9] * y + m[10] * z + m[11])
        r, g, b, a = clr[ai]
        ecol = (a << 24) | (b << 16) | (g << 8) | r
        eu, ev = tex[ai][0] / 256.0, tex[ai][1] / 256.0
        u, v, col, gx, gy, gz = verts[k]
        check(abs(gx - ex[0]) < 1e-3 and abs(gy - ex[1]) < 1e-3 and abs(gz - ex[2]) < 1e-3, f"DL vertex {k}: pos {gx, gy, gz} vs {ex}")
        check(col == ecol, f"DL vertex {k}: colour {col:08x} vs {ecol:08x}")
        check(abs(u - eu) < 1e-4 and abs(v - ev) < 1e-4, f"DL vertex {k}: tex {u, v} vs {eu, ev}")
    print("  display list (indexed s16/s8/rgba8/s16, matrix indices) ok")
    # quads with direct big-endian floats and colours, vertex format 1
    q = [(rnd.uniform(-5, 5), rnd.uniform(-5, 5), rnd.uniform(-5, 5)) for _ in range(4)]
    qc = [(rnd.randrange(256),) * 4 for _ in range(4)]
    dl = bytearray([0x81]) + struct.pack(">H", 4)
    for (x, y, z), c in zip(q, qc):
        dl += struct.pack(">fff", x, y, z) + bytes(c)
    script = ("clearvcd\nvcd 9 1\nvcd 11 1\nvat 1 9 1 4 0\nvat 1 11 1 5 0\nmatsrc 1\nnumtexgens 0\n"
              "posmtx 0 1 0 0 0 0 1 0 0 0 0 1 0\ncurmtx 0\n" + f"dl {dl.hex()}\n")
    prim, verts, _ = parse_draws(run(script))[0]
    check(prim == 3 and len(verts) == 6, f"quads: prim {prim} count {len(verts)}")
    for k, src in enumerate([0, 1, 2, 0, 2, 3]):
        u, v, col, gx, gy, gz = verts[k]
        check(abs(gx - q[src][0]) < 1e-4 and abs(gy - q[src][1]) < 1e-4 and abs(gz - q[src][2]) < 1e-4, f"quad vertex {k}")
    print("  quads (direct big-endian floats) ok")


def test_immediate_and_texmtx(rnd):
    tri = [(1.0, 2.0, 3.0), (-4.5, 0.25, 8.0), (7.0, -1.0, 2.5)]
    st = [(0.0, 0.0), (1.0, 0.0), (0.5, 1.0)]
    script = ("clearvcd\nvcd 9 1\nvcd 13 1\nvat 2 9 1 4 0\nvat 2 13 1 4 0\nmatsrc 0\nmatcolor 10 20 30 40\nnumtexgens 1\n"
              "texmtx 30 2 0 0 0.5 0 3 0 0.25\ntexgen 30\nposmtx 6 1 0 0 100 0 1 0 0 0 0 1 0\ncurmtx 6\nbegin 144 2 3\n")
    for (x, y, z), (s, t) in zip(tri, st):
        script += f"putf {x}\nputf {y}\nputf {z}\nputf {s}\nputf {t}\n"
    prim, verts, _ = parse_draws(run(script))[0]
    check(prim == 3 and len(verts) == 3, f"immediate: prim {prim} count {len(verts)}")
    for k in range(3):
        u, v, col, gx, gy, gz = verts[k]
        check(abs(gx - (tri[k][0] + 100)) < 1e-4 and abs(gy - tri[k][1]) < 1e-4 and abs(gz - tri[k][2]) < 1e-4, f"immediate vertex {k} pos")
        check(abs(u - (2 * st[k][0] + 0.5)) < 1e-5 and abs(v - (3 * st[k][1] + 0.25)) < 1e-5, f"immediate vertex {k} texmtx {u, v}")
        check(col == (40 << 24) | (30 << 16) | (20 << 8) | 10, f"immediate vertex {k}: material colour {col:08x}")
    print("  immediate mode (direct floats, texture matrix, material colour) ok")
    lines = run("proj 0 1.5 0 0 0 0 2 0 0 0 0 -1.25 -3 0 0 -1 0\n")
    proj = [l for l in lines if l.startswith("PROJ ")][-1].split()[1:]
    got = [float(x) for x in proj]
    check(got[8:12] == [0.0, 0.0, -3.5, -6.0] and got[12:16] == [0.0, 0.0, -1.0, 0.0], f"projection z fix: {got}")
    print("  projection z range fix ok")


def state_lines(lines, prefix):
    return [l for l in lines if l.startswith(prefix + " ")]


def test_render_state():
    # GX compare functions never, less, equal, lequal, greater, nequal, gequal, always -> GE ids
    lines = run("".join(f"zmode 1 {f} 1\n" for f in range(8)) + "zmode 0 3 0\n")
    got = [l.split()[1:] for l in state_lines(lines, "DEPTH")]
    check(got == [["1", str(g), "1"] for g in [0, 4, 2, 5, 6, 3, 7, 1]] + [["0", "5", "0"]], f"depth mapping {got}")
    # blend: GX_BM_BLEND src alpha / inv src alpha, GX_BM_BLEND one / one (fixed colours), subtract, none
    lines = run("blend 1 4 5 0\nblend 1 1 1 0\nblend 3 0 0 0\nblend 0 4 5 0\n")
    got = [l.split()[1:] for l in state_lines(lines, "BLEND")]
    check(got == [["1", "0", "2", "3", "000000", "000000"], ["1", "0", "10", "10", "ffffff", "ffffff"],
                  ["1", "2", "10", "10", "ffffff", "ffffff"], ["0", "0", "0", "0", "000000", "000000"]], f"blend mapping {got}")
    lines = run("cull 0\ncull 1\ncull 2\n")
    got = [l.split()[1:] for l in state_lines(lines, "CULL")]
    check(got == [["0", "0"], ["1", "0"], ["1", "1"]], f"cull mapping {got}")
    # GX_CULL_ALL draws nothing
    lines = run("cull 3\n" + point_setup() + "begin 184 0 1\nputf 1\nputf 2\nputf 3\ncull 0\nbegin 184 0 1\nputf 1\nputf 2\nputf 3\n")
    check(len(parse_draws(lines)) == 1, "GX_CULL_ALL still drew")
    lines = run("alphacmp 4 128 0 7 0\nalphacmp 7 0 0 7 0\n")
    got = [l.split()[1:] for l in state_lines(lines, "ALPHA")]
    check(got == [["1", "6", "128"], ["0", "0", "0"]], f"alpha test mapping {got}")
    lines = run("colorupdate 0\nalphaupdate 0\ncolorupdate 1\nalphaupdate 1\n")
    got = [l.split()[1] for l in state_lines(lines, "MASK")]
    check(got == ["00ffffff", "ffffffff", "ff000000", "00000000"], f"pixel mask {got}")
    lines = run("fog 2 10 200 1 1000 12 34 56 255\nfog 0 0 0 0 0 0 0 0 0\n")
    got = [l.split()[1:] for l in state_lines(lines, "FOG")]
    check(got == [["1", "10", "200", "38220c"], ["0", "0", "0", "000000"]], f"fog mapping {got}")
    # texture state: wrap clamp/repeat, filters, tev op
    rnd = random.Random(5)
    data, _, _, _ = make_texture(1, 8, 8, rnd)
    lines = run(point_setup() + f"texobj 1 8 8 {data.hex()}\ntevop 0 3\nbegin 184 0 1\nputf 1\nputf 2\nputf 3\n")
    got = state_lines(lines, "TEXSTATE")[-1].split()[1:]
    check(got == ["0", "0", "1", "1", "3"], f"texture state {got}")
    print("  render state (depth, blend, cull, alpha test, masks, fog, tex state) ok")


def test_primitives(rnd):
    # a strip and a fan reach the GE as strips and fans; lines and points keep their kind
    for gxprim, name, pgprim, n in [(0x98, "strip", 4, 5), (0xA0, "fan", 5, 5), (0xA8, "lines", 1, 4), (0xB0, "line strip", 2, 3), (0xB8, "points", 0, 2), (0x90, "triangles", 3, 6)]:
        pts = [(rnd.uniform(-9, 9), rnd.uniform(-9, 9), rnd.uniform(-9, 9)) for _ in range(n)]
        script = "clearvcd\nvcd 9 1\nvat 0 9 1 4 0\nnumtexgens 0\nposmtx 0 1 0 0 0 0 1 0 0 0 0 1 0\ncurmtx 0\n" + f"begin {gxprim} 0 {n}\n"
        script += "".join(f"putf {x}\nputf {y}\nputf {z}\n" for x, y, z in pts)
        prim, verts, _ = parse_draws(run(script))[0]
        check(prim == pgprim and len(verts) == n, f"{name}: prim {prim} count {len(verts)}")
        for k in range(n):
            check(all(abs(verts[k][3 + i] - pts[k][i]) < 1e-4 for i in range(3)), f"{name} vertex {k}")
    print("  primitive kinds (strip, fan, lines, line strip, points, triangles) ok")
    # immediate mode with s16 positions (frac 4), s8 normals, u8 colour bytes, s16 texcoords: the
    # message / debug text path (mes.cpp, eprintf.cpp)
    verts = [(rnd.randrange(-3000, 3000), rnd.randrange(-3000, 3000), rnd.randrange(-3000, 3000),
              rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), rnd.randrange(256),
              rnd.randrange(-1000, 1000), rnd.randrange(-1000, 1000)) for _ in range(4)]
    script = ("clearvcd\nvcd 9 1\nvcd 10 1\nvcd 11 1\nvcd 13 1\nvat 3 9 1 3 4\nvat 3 10 0 1 0\nvat 3 11 1 5 0\nvat 3 13 1 3 7\n"
              "matsrc 1\nnumtexgens 1\ntexgen 60\nposmtx 0 1 0 0 0 0 1 0 0 0 0 1 0\ncurmtx 0\nbegin 128 3 4\n")
    for x, y, z, r, g, b, a, s, t in verts:
        script += f"puts16 {x}\nputs16 {y}\nputs16 {z}\nputs8 1\nputs8 2\nputs8 3\nput8 {r}\nput8 {g}\nput8 {b}\nput8 {a}\nputs16 {s}\nputs16 {t}\n"
    prim, out, _ = parse_draws(run(script))[0]
    check(prim == 3 and len(out) == 6, f"immediate quads: prim {prim} count {len(out)}")
    for k, src in enumerate([0, 1, 2, 0, 2, 3]):
        x, y, z, r, g, b, a, s, t = verts[src]
        u, v, col, gx, gy, gz = out[k]
        check(abs(gx - x / 16) < 1e-4 and abs(gy - y / 16) < 1e-4 and abs(gz - z / 16) < 1e-4, f"immediate s16 vertex {k}: {gx, gy, gz} vs {x / 16, y / 16, z / 16}")
        check(col == (a << 24) | (b << 16) | (g << 8) | r, f"immediate colour bytes {k}: {col:08x}")
        check(abs(u - s / 128) < 1e-5 and abs(v - t / 128) < 1e-5, f"immediate s16 texcoord {k}: {u, v} vs {s / 128, t / 128}")
    print("  immediate mode s16/s8/u8 colour bytes/s16 texcoords ok")
    # colour formats in a disc display list: RGB565, RGB8, RGBX8, RGBA4, RGBA6
    cases = [(0, lambda: struct.pack(">H", rnd.randrange(65536))), (1, lambda: bytes(rnd.randrange(256) for _ in range(3))),
             (2, lambda: bytes(rnd.randrange(256) for _ in range(4))), (3, lambda: struct.pack(">H", rnd.randrange(65536))),
             (4, lambda: bytes(rnd.randrange(256) for _ in range(3)))]
    for ctype, gen in cases:
        cols = [gen() for _ in range(3)]
        dl = bytearray([0x92]) + struct.pack(">H", 3)
        for c in cols:
            dl += struct.pack(">fff", 1, 2, 3) + c
        script = f"clearvcd\nvcd 9 1\nvcd 11 1\nvat 2 9 1 4 0\nvat 2 11 1 {ctype} 0\nmatsrc 1\nnumtexgens 0\nposmtx 0 1 0 0 0 0 1 0 0 0 0 1 0\ncurmtx 0\ndl {dl.hex()}\n"
        prim, out, _ = parse_draws(run(script))[0]
        check(len(out) == 3, f"colour type {ctype}: count {len(out)}")
        for k, c in enumerate(cols):
            if ctype == 0:
                v = struct.unpack(">H", c)[0]; r, g, b, a = (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3, 255
            elif ctype in (1, 2):
                r, g, b, a = c[0], c[1], c[2], 255
            elif ctype == 3:
                v = struct.unpack(">H", c)[0]; r, g, b, a = [((v >> sh) & 15) * 17 for sh in (12, 8, 4, 0)]
            else:
                v = (c[0] << 16) | (c[1] << 8) | c[2]; r, g, b, a = [((v >> sh) & 63) << 2 for sh in (18, 12, 6, 0)]
            check(out[k][2] == (a << 24) | (b << 16) | (g << 8) | r, f"colour type {ctype} vertex {k}: {out[k][2]:08x} vs {a, b, g, r}")
    print("  display-list colour formats (565, rgb8, rgbx8, rgba4, rgba6) ok")


def test_dl_registers(rnd):
    # a display list that sets its own VCD / VAT / strides through CP registers before drawing:
    # POS index16 s16 frac 2, CLR0 index8 rgba8, TEX0 index16 u16 frac 6, on vertex format 5
    pos = [(rnd.randrange(-2000, 2000), rnd.randrange(-2000, 2000), rnd.randrange(-2000, 2000)) for _ in range(4)]
    clr = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(4)]
    tex = [(rnd.randrange(0, 60000), rnd.randrange(0, 60000)) for _ in range(4)]
    pos_bytes = b"".join(be_s16(x) + be_s16(y) + be_s16(z) + b"\0\0" for x, y, z in pos)  # stride 8
    clr_bytes = b"".join(bytes(c) for c in clr)
    tex_bytes = b"".join(struct.pack(">HH", s, t) for s, t in tex)
    vcd_lo = (3 << 9) | (2 << 13)
    vcd_hi = 3
    vat_a = 1 | (3 << 1) | (2 << 4) | (1 << 13) | (5 << 14) | (1 << 21) | (2 << 22) | (6 << 25) | (1 << 30)
    dl = bytearray()
    dl += bytes([0x08, 0x50]) + struct.pack(">I", vcd_lo)
    dl += bytes([0x08, 0x60]) + struct.pack(">I", vcd_hi)
    dl += bytes([0x08, 0x75]) + struct.pack(">I", vat_a)
    dl += bytes([0x08, 0xB0]) + struct.pack(">I", 8)
    dl += bytes([0x08, 0xB2]) + struct.pack(">I", 4)
    dl += bytes([0x08, 0xB4]) + struct.pack(">I", 4)
    dl += bytes([0x10]) + struct.pack(">HH", 2, 0x1018) + b"\0" * 12  # an XF load of 3 words: skipped
    dl += bytes([0x61]) + b"\0\0\0\0"  # a BP load: skipped
    dl += bytes([0x40])  # invalidate vertex cache
    dl += bytes([0x9D]) + struct.pack(">H", 4)
    for i in (3, 1, 2, 0):
        dl += struct.pack(">H", i) + bytes([i]) + struct.pack(">H", i)
    script = ("clearvcd\nvcd 9 1\nvat 5 9 1 4 0\n"  # the CP registers in the list override this
              f"array 9 1 {pos_bytes.hex()}\narray 11 1 {clr_bytes.hex()}\narray 13 1 {tex_bytes.hex()}\n"
              "matsrc 1\nnumtexgens 1\ntexgen 60\nposmtx 0 1 0 0 0 0 1 0 0 0 0 1 0\ncurmtx 0\n" + f"dl {dl.hex()}\n")
    draws = parse_draws(run(script))
    check(len(draws) == 1, f"CP registers: {len(draws)} draws")
    prim, out, _ = draws[0]
    check(prim == 4 and len(out) == 4, f"CP registers: prim {prim} count {len(out)}")
    for k, i in enumerate((3, 1, 2, 0)):
        u, v, col, gx, gy, gz = out[k]
        check(abs(gx - pos[i][0] / 4) < 1e-4 and abs(gy - pos[i][1] / 4) < 1e-4 and abs(gz - pos[i][2] / 4) < 1e-4, f"CP vertex {k} pos {gx, gy, gz} vs {[c / 4 for c in pos[i]]}")
        r, g, b, a = clr[i]
        check(col == (a << 24) | (b << 16) | (g << 8) | r, f"CP vertex {k} colour")
        check(abs(u - tex[i][0] / 64) < 1e-3 and abs(v - tex[i][1] / 64) < 1e-3, f"CP vertex {k} tex {u, v} vs {tex[i][0] / 64, tex[i][1] / 64}")
    print("  display-list CP registers (VCD, VAT, strides), XF/BP skips ok")


def test_recording(rnd):
    # GXBeginDisplayList / GXEndDisplayList capture immediate draws; GXCallDisplayList replays them
    tri = [(rnd.uniform(-5, 5), rnd.uniform(-5, 5), rnd.uniform(-5, 5)) for _ in range(3)]
    script = "clearvcd\nvcd 9 1\nvcd 11 1\nvat 0 9 1 4 0\nvat 0 11 1 5 0\nmatsrc 1\nnumtexgens 0\nposmtx 0 1 0 0 0 0 1 0 0 0 0 1 0\ncurmtx 0\nbeginlist 4096\nbegin 144 0 3\n"
    for k, (x, y, z) in enumerate(tri):
        script += f"putf {x}\nputf {y}\nputf {z}\nput8 {k}\nput8 {k + 1}\nput8 {k + 2}\nput8 255\n"
    script += "endlist\nclearvcd\nvcd 9 1\nvat 0 9 1 3 0\ncalllist\ncalllist\n"  # the VCD/VAT changes must not affect the replay
    lines = run(script)
    rec = [l for l in lines if l.startswith("RECORDED ")]
    check(len(rec) == 1 and int(rec[0].split()[1]) % 32 == 0, f"recording length {rec}")
    draws = parse_draws(lines)
    check(len(draws) == 2, f"recording replayed {len(draws)} times")
    for prim, out, _ in draws:
        check(prim == 3 and len(out) == 3, "replay prim/count")
        for k in range(3):
            check(all(abs(out[k][3 + i] - tri[k][i]) < 1e-4 for i in range(3)), f"replay vertex {k} pos")
            check(out[k][2] == (255 << 24) | ((k + 2) << 16) | ((k + 1) << 8) | k, f"replay vertex {k} colour {out[k][2]:08x}")
    print("  recorded display list (GXBeginDisplayList / GXCallDisplayList) ok")


def test_lighting(rnd):
    import math
    # two point lights on a lit triangle with s8 normals (frac 6), the normal matrix a rotation,
    # spot / distance attenuation from the SDK tables, ambient from the register
    def e(v): return max(0.0, min(1.0, v))
    tri = [((rnd.uniform(-5, 5), rnd.uniform(-5, 5), rnd.uniform(-5, 5)), (rnd.randrange(-64, 65), rnd.randrange(-64, 65), rnd.randrange(-64, 65))) for _ in range(3)]
    mat = (200, 150, 100, 220)
    amb = (20, 30, 40, 0)
    l0 = dict(col=(255, 128, 64), pos=(3.0, 4.0, -2.0), dir=(0.0, 0.0, 1.0), a=(1.0, 0.0, 0.0), k=(1.0, 0.0, 0.0))
    cutoff, ref, br = 30.0, 20.0, 0.25
    cr = math.cos(math.radians(cutoff)); d1 = (1 - cr) ** 2
    l1 = dict(col=(0, 200, 255), pos=(-2.0, 1.0, 5.0), dir=(0.6, 0.0, -0.8),
              a=(cr * (cr - 2) / d1, 2 / d1, -1 / d1),                                    # GX_SP_SHARP
              k=(1.0, 0.5 * (1 - br) / (br * ref), 0.5 * (1 - br) / (br * ref * ref)))    # GX_DA_MEDIUM
    ang = 0.7
    nm = [math.cos(ang), 0, math.sin(ang), 0, 1, 0, -math.sin(ang), 0, math.cos(ang)]
    script = ("clearvcd\nvcd 9 1\nvcd 10 1\nvat 0 9 1 4 0\nvat 0 10 0 1 0\nnumtexgens 0\nposmtx 0 1 0 0 0 0 1 0 0 0 0 1 0\ncurmtx 0\n"
              f"nrmmtx 0 {' '.join(str(x) for x in nm)}\n"
              f"matcolor {' '.join(map(str, mat))}\nambcolor {' '.join(map(str, amb))}\n"
              f"light 1 {' '.join(map(str, l0['col']))} 255 {' '.join(map(str, l0['pos'] + l0['dir'] + l0['a'] + l0['k']))}\n"
              f"lightspot 2 {' '.join(map(str, l1['col']))} 255 {' '.join(map(str, l1['pos'] + l1['dir']))} {cutoff} 4 {ref} {br} 2\n"
              "chanctrl 0 1 0 0 3 2 1\nchanctrl 2 0 0 0 0 2 2\nbegin 144 0 3\n")
    for (x, y, z), (nx, ny, nz) in tri:
        script += f"putf {x}\nputf {y}\nputf {z}\nputs8 {nx}\nputs8 {ny}\nputs8 {nz}\n"
    prim, verts, _ = parse_draws(run(script))[0]
    check(len(verts) == 3, "lighting: count")
    for k, ((x, y, z), (nx, ny, nz)) in enumerate(tri):
        n = (nx / 64, ny / 64, nz / 64)
        tn = (nm[0] * n[0] + nm[1] * n[1] + nm[2] * n[2], nm[3] * n[0] + nm[4] * n[1] + nm[5] * n[2], nm[6] * n[0] + nm[7] * n[1] + nm[8] * n[2])
        ln = math.sqrt(sum(c * c for c in tn)) or 1.0
        tn = tuple(c / ln for c in tn)
        il = [amb[0] / 255, amb[1] / 255, amb[2] / 255]
        for l in (l0, l1):
            lv = (l['pos'][0] - x, l['pos'][1] - y, l['pos'][2] - z)
            d = math.sqrt(sum(c * c for c in lv)); lv = tuple(c / d for c in lv)
            diff = max(0.0, sum(a * b for a, b in zip(tn, lv)))
            cosa = -sum(a * b for a, b in zip(lv, l['dir']))
            aatt = max(0.0, l['a'][0] + l['a'][1] * cosa + l['a'][2] * cosa * cosa)
            den = l['k'][0] + l['k'][1] * d + l['k'][2] * d * d
            att = aatt / den if den > 0 else 0.0
            kk = att * diff
            for i in range(3):
                il[i] += kk * l['col'][i] / 255
        exp = [int(mat[i] * e(il[i]) + 0.5) for i in range(3)] + [mat[3]]
        got = verts[k][2]
        gr, gg, gb, ga = got & 255, (got >> 8) & 255, (got >> 16) & 255, got >> 24
        check(all(abs(a - b) <= 1 for a, b in zip((gr, gg, gb, ga), exp)), f"lighting vertex {k}: {(gr, gg, gb, ga)} vs {exp}")
    print("  vertex lighting (point + spot lights, normal matrix, ambient, material) ok")


def test_copies():
    # an EFB copy (640x448 source, half-size RGBA8 destination) becomes an 8888 texture of the copy's
    # size at the destination address; the A8 and Z8 formats pick the alpha / depth read-backs
    script = point_setup() + "copysrc 0 0 640 448\ncopydst 320 224 6 1\ncopytex 0\ntexobjcopy 6 320 224\nbegin 184 0 1\nputf 1\nputf 2\nputf 3\n"
    lines = run(script)
    copy = [l for l in lines if l.startswith("COPY ")]
    check(copy == ["COPY 320 224 0 0 480 272 0"], f"copy request {copy}")
    psm, w, h, out = parse_draws(lines)[0][2]
    check(psm == 3 and (w, h) == (320, 224), f"copy texture {psm} {w}x{h}")
    check(struct.unpack_from("<I", out, 0)[0] == 0xFF000000 and struct.unpack_from("<I", out, 4 * 1000)[0] == 0xFF000000 + 1000, "copy texture is the read-back")
    lines = run(point_setup() + "copysrc 0 0 128 128\ncopydst 64 64 39 0\ncopytex 1\ncopydst 64 64 17 1\ncopytex 0\n")
    copy = [l for l in lines if l.startswith("COPY ") or l.startswith("CLEAR ")]
    check(copy == ["COPY 64 64 0 0 96 77 1", "CLEAR ff000000 65535", "COPY 64 64 0 0 96 77 2"], f"alpha / depth copies {copy}")
    print("  framebuffer copies (RGBA8 half size, A8, Z8, clear after copy) ok")


def main():
    build()
    rnd = random.Random(11)
    test_textures(rnd)
    test_display_list(rnd)
    test_immediate_and_texmtx(rnd)
    test_render_state()
    test_primitives(rnd)
    test_dl_registers(rnd)
    test_recording(rnd)
    test_lighting(rnd)
    test_copies()
    print("gxtest ok")


if __name__ == "__main__":
    main()
