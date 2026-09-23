#!/usr/bin/env python3
"""Capcom's yz2 archive codec (the room archives "stX/rNNN.das" and the other .das files).

The format, read from game/yz2asm.cpp (assembly) and game/yz2code.cpp (set-up): a text header
"<packed hex> <unpacked hex>", then, at the next 32-byte boundary, an adaptive range-coded stream.
Two models: the main one over 0x500 symbols (0x000..0x1FF a short dictionary reference with an
explicit length, 0x200..0x3FF a long reference reusing the stored run length, 0x400..0x4FF a
literal byte) and a 0x100-symbol one for the length fields. Every run written is entered into a
512-entry ring of (start, length) runs kept per context byte, the byte before the run; a reference
names a ring slot relative to the ring's write index.

    yz2.py decode in.das out.bin        # unpack
    yz2.py encode in.bin out.das        # pack (a plain greedy matcher; only for tests and tools)
    yz2.py raw in.bin out.das           # wrap unpacked data as a raw archive (packed size 0), the
                                        # form port/src/port_yz2.cpp copies without decoding
    yz2.py selftest                     # round trips, and the C decoder against this one

The C decoder for the port is port/src/port_yz2.cpp; `selftest` compiles it on the host.
"""
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Model:
    """One adaptive frequency model, exactly as yz2code.cpp builds it and yz2asm.cpp updates it."""

    def __init__(self, n):
        self.n = n
        freq = [0] * n
        j = 0
        for _ in range(0x8000):
            freq[j] += 1
            j += 1
            if j >= n:
                j = 0
        self.cum_fr = list(freq)
        self.cum_lo = []
        s = 0
        for i in range(n):
            self.cum_lo.append(s)
            s += freq[i]
        # FREQ_RESET
        self.freq = [1] * n
        self.bits = 0
        self.max = n
        if self.max >= 1:
            while True:
                self.bits += 1
                if self.bits > 14:
                    break
                if not (self.max >= (1 << self.bits)):
                    break
        self.range = 1 << self.bits
        self.table = None

    def build_table(self):
        t = [0] * 0x8000
        for i in range(self.n):
            lo = self.cum_lo[i]
            for j in range(lo, lo + self.cum_fr[i]):
                t[j] = i
        self.table = t

    def update(self, sym):
        self.freq[sym] += 1
        self.max += 1
        if self.bits <= 14:
            if self.max == self.range:
                mult = 1 << (15 - self.bits)
                acc = 0
                self.table = None
                for i in range(self.n):
                    v = (self.freq[i] * mult) & 0xFFFF
                    self.cum_lo[i] = acc & 0xFFFF
                    acc += v
                    self.cum_fr[i] = v
                self.bits += 1
                self.range = 1 << self.bits
        elif self.max > 0x7FFF:
            acc = 0
            self.table = None
            self.max = 0
            for i in range(self.n):
                v = self.freq[i]
                self.cum_fr[i] = v
                self.cum_lo[i] = acc & 0xFFFF
                acc += v
                if v > 1:
                    v >>= 1
                    self.freq[i] = v
                self.max += v


class Decoder:
    def __init__(self, data, pos):
        self.data = data
        self.pos = pos + 1
        self.R = 0x80
        self.C = data[pos]
        self.m1 = Model(0x500)
        self.m2 = Model(0x100)

    def symbol(self, m):
        R, C = self.R, self.C
        d = self.data
        if R <= 0x800000:
            if R > 0x8000:
                C = ((C << 8) | d[self.pos]) & 0xFFFFFFFF
                self.pos += 1
                R = (R << 8) & 0xFFFFFFFF
            elif R > 0x80:
                C = ((C << 16) | (d[self.pos] << 8) | d[self.pos + 1]) & 0xFFFFFFFF
                self.pos += 2
                R = (R << 16) & 0xFFFFFFFF
            else:
                C = ((C << 24) | (d[self.pos] << 16) | (d[self.pos + 1] << 8) | d[self.pos + 2]) & 0xFFFFFFFF
                self.pos += 3
                R = (R << 24) & 0xFFFFFFFF
        R >>= 14
        target = C // R
        if m.table is None:
            m.build_table()
        sym = m.table[target]
        fr, lo = m.cum_fr[sym], m.cum_lo[sym]
        self.C = (C - R * lo) & 0xFFFFFFFF
        self.R = (R * fr) >> 1
        m.update(sym)
        return sym

    def decode(self, size):
        out = bytearray()
        dic = [[] for _ in range(256)]  # per context: list of (start, length), ring of 512
        cnt = [0] * 256
        scan = 0
        while len(out) < size:
            sym = self.symbol(self.m1)
            if sym > 0x3FF:
                out.append(sym & 0xFF)
                length = 1
            else:
                b = out[scan]
                ring = dic[b]
                idx = (sym + cnt[b]) & 0x1FF
                if sym > 0x1FF:
                    start, length = ring[idx] if idx < len(ring) else (0, 0)
                else:
                    t = self.symbol(self.m2)
                    if t > 2:
                        length = t
                    elif t == 2:
                        length = (self.symbol(self.m2) << 8) | self.symbol(self.m2)
                    elif t == 1:
                        a = self.symbol(self.m2) << 16
                        a |= self.symbol(self.m2) << 8
                        length = a | self.symbol(self.m2)
                    else:
                        a = self.symbol(self.m2) << 24
                        a |= self.symbol(self.m2) << 16
                        a |= self.symbol(self.m2) << 8
                        length = a | self.symbol(self.m2)
                    start = ring[idx][0] if idx < len(ring) else 0
                    length -= 1
                for i in range(length):
                    out.append(out[start + i])
            if scan < len(out) - 1:
                b = out[scan]
                start = scan + 1
                scan = len(out) - 1
                ring = dic[b]
                c = cnt[b]
                if c < len(ring):
                    ring[c] = (start, length)
                else:
                    ring.append((start, length))
                cnt[b] = (c + 1) & 0x1FF
        return bytes(out)


class Encoder:
    """The mirror image of Decoder: the same models, the same renormalisation points."""

    def __init__(self):
        self.low = 0          # the code value, unbounded
        self.R = 0x80
        self.shifted = 0      # bytes shifted out so far
        self.m1 = Model(0x500)
        self.m2 = Model(0x100)

    def symbol(self, m, sym):
        R = self.R
        if R <= 0x800000:
            if R > 0x8000:
                k = 1
            elif R > 0x80:
                k = 2
            else:
                k = 3
            self.low <<= 8 * k
            R = (R << (8 * k)) & 0xFFFFFFFF
            self.shifted += k
        R >>= 14
        fr, lo = m.cum_fr[sym], m.cum_lo[sym]
        self.low += R * lo
        self.R = (R * fr) >> 1
        m.update(sym)

    def finish(self):
        # the decoder reads one byte at set-up and four ahead of its last shift
        n = 1 + self.shifted + 4
        return (self.low << 32).to_bytes(n, "big")


def encode(data, max_candidates=64):
    enc = Encoder()
    dic = [[] for _ in range(256)]
    cnt = [0] * 256
    out_len = 0
    scan = 0
    n = len(data)
    while out_len < n:
        pos = out_len
        best = None  # (kind, sym, length)
        b = data[scan]
        ring = dic[b]
        if pos > 0 and ring:
            c = cnt[b]
            order = list(range(len(ring)))
            order.sort(key=lambda e: (c - 1 - e) & 0x1FF)  # most recent first
            for e in order[:max_candidates]:
                start, length = ring[e]
                m = 0
                lim = n - pos
                while m < lim and data[pos + m] == data[start + m]:
                    m += 1
                    if m >= 0xFFFFFF:
                        break
                if m == 0:
                    continue
                if m >= length >= 2 and (best is None or length > best[2]):
                    best = ("long", 0x200 + ((e - c) & 0x1FF), length)
                if m >= 3 and (best is None or m > best[2] + 1):
                    best = ("short", (e - c) & 0x1FF, m)
        if best is None:
            enc.symbol(enc.m1, 0x400 + data[pos])
            length = 1
        else:
            kind, sym, length = best
            enc.symbol(enc.m1, sym)
            if kind == "short":
                t = length + 1
                if 2 < t <= 0xFF:
                    enc.symbol(enc.m2, t)
                elif t <= 0xFFFF:
                    enc.symbol(enc.m2, 2)
                    enc.symbol(enc.m2, t >> 8)
                    enc.symbol(enc.m2, t & 0xFF)
                elif t <= 0xFFFFFF:
                    enc.symbol(enc.m2, 1)
                    enc.symbol(enc.m2, t >> 16)
                    enc.symbol(enc.m2, (t >> 8) & 0xFF)
                    enc.symbol(enc.m2, t & 0xFF)
                else:
                    enc.symbol(enc.m2, 0)
                    enc.symbol(enc.m2, t >> 24)
                    enc.symbol(enc.m2, (t >> 16) & 0xFF)
                    enc.symbol(enc.m2, (t >> 8) & 0xFF)
                    enc.symbol(enc.m2, t & 0xFF)
        out_len += length
        if scan < out_len - 1:
            b = data[scan]
            start = scan + 1
            scan = out_len - 1
            ring = dic[b]
            c = cnt[b]
            if c < len(ring):
                ring[c] = (start, length)
            else:
                ring.append((start, length))
            cnt[b] = (c + 1) & 0x1FF
    return enc.finish()


# ---- the archive envelope
def parse_header(blob):
    """(packed size, unpacked size, stream offset) of a .das archive."""
    i = 0
    while i < len(blob) and blob[i] in b" \t\r\n":
        i += 1
    j = i
    while j < len(blob) and chr(blob[j]) in "0123456789abcdefABCDEF":
        j += 1
    packed = int(blob[i:j], 16)
    j += 1  # the separator
    k = j
    while k < len(blob) and chr(blob[k]) in "0123456789abcdefABCDEF":
        k += 1
    unpacked = int(blob[j:k], 16)
    return packed, unpacked, (k + 0x20) & ~0x1F


def wrap(packed, unpacked, stream):
    header = f"{packed:X} {unpacked:X}".encode()
    ofs = (len(header) + 0x20) & ~0x1F
    return header + b"\0" * (ofs - len(header)) + stream


def decode_archive(blob):
    packed, unpacked, ofs = parse_header(blob)
    if packed == 0:
        return bytes(blob[ofs:ofs + unpacked])
    return Decoder(blob, ofs).decode(unpacked)


def encode_archive(data):
    stream = encode(data)
    return wrap(len(stream), len(data), stream)


def raw_archive(data):
    return wrap(0, len(data), data)


# ---- self-test: round trips, and the port's C decoder against this one
HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef int s32;
extern "C" void yz2Decode_Decode(void* ctx, void* dst, u32 size, void* ev);
struct Yz2InEv { u8* src; u8* heap; u8* free; u32 size0; u32 size1; };
struct Yz2Freq { u16* freq; u32 max; int bits; u32 range; u16* cum; int n; u32 acc; };
struct Yz2Dec; struct Yz2Model { Yz2Dec* dec; Yz2Freq fd; u8* table; };
struct Yz2Dec { u32 mask; u32 byte; Yz2Model m1; Yz2Model m2; };
struct Yz2DicEnt { u32 cnt; u32 a[0x200]; u32 b[0x200]; };
struct Yz2Ctx { Yz2DicEnt* dic; Yz2Dec d; u32 pad_54; int n; };
static void freqReset(Yz2Freq* f) {
    for (int i = 0; i < f->n; i++) f->freq[i] = 1;
    f->bits = 0; f->max = f->n;
    if (f->max >= 1u) { do { f->bits++; if (f->bits > 14) break; } while (f->max >= (1u << f->bits)); }
    f->range = 1u << f->bits; f->acc = 0;
}
static void modelSetup(Yz2Model* m, Yz2Dec* d, int cnt) {
    Yz2Freq* fd = &m->fd; m->dec = d; fd->n = cnt;
    fd->freq = (u16*) calloc(cnt, 2); fd->cum = (u16*) calloc(cnt, 4);
    int j = 0; for (int i = 0; i < 0x8000; i++) { fd->freq[j]++; if (++j >= cnt) j = 0; }
    u16 s = 0; for (int i = 0; i < cnt; i++) { fd->cum[i * 2] = fd->freq[i]; fd->cum[i * 2 + 1] = s; s += fd->freq[i]; }
    freqReset(fd); m->table = (u8*) calloc(0x20000, 1);
}
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8* blob = (u8*) malloc(n + 16); fread(blob, 1, n, f); fclose(f);
    u32 packed = strtoul((char*) blob, NULL, 16); char* p = (char*) blob; strtoul(p, &p, 16); p++;
    u32 unpacked = strtoul(p, &p, 16); u8* src = blob + ((((u8*) p - blob) + 0x20) & ~0x1F);
    Yz2InEv ev; ev.src = src; ev.size0 = packed; ev.size1 = unpacked;
    Yz2Ctx ctx; memset(&ctx, 0, sizeof(ctx)); Yz2Dec* d = &ctx.d;
    d->mask = 0x80; d->byte = *ev.src++; ctx.n = 0x500;
    modelSetup(&d->m1, d, 0x500); modelSetup(&d->m2, d, 0x100);
    ctx.dic = (Yz2DicEnt*) calloc(0x100, sizeof(Yz2DicEnt));
    freqReset(&d->m1.fd); freqReset(&d->m2.fd);
    u8* out = (u8*) malloc(unpacked + 16);
    yz2Decode_Decode(&ctx, out, unpacked, &ev);
    f = fopen(argv[2], "wb"); fwrite(out, 1, unpacked, f); fclose(f);
    return 0;
}
"""


def selftest():
    rnd = random.Random(4)
    samples = []
    samples.append(b"")
    samples.append(b"A")
    samples.append(b"abcabcabcabcabcabc" * 4)
    samples.append(bytes(rnd.randrange(256) for _ in range(3000)))
    chunk = bytes(rnd.randrange(256) for _ in range(700))
    samples.append(chunk + b"\0" * 50 + chunk + chunk[:333] + b"x" * 300 + chunk)  # short refs incl. > 255
    samples.append(b"\x55" * 70000 + b"end")  # a run beyond 65535: the 24-bit length form
    words = [bytes(rnd.randrange(97, 123) for _ in range(rnd.randrange(3, 9))) for _ in range(40)]
    samples.append(b" ".join(rnd.choice(words) for _ in range(6000)))
    for i, s in enumerate(samples):
        arc = encode_archive(s)
        back = decode_archive(arc)
        assert back == s, f"sample {i}: round trip failed ({len(back)} vs {len(s)} bytes)"
        assert decode_archive(raw_archive(s)) == s
        print(f"sample {i}: {len(s)} -> {len(arc)} bytes ok")
    # the port's C decoder on the host
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / "types.h").write_text("typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef int s32;\n")
        (d / "port.h").write_text("")
        (d / "harness.cpp").write_text(HARNESS)
        exe = d / "yz2test"
        cc = ["clang++", "-O1", "-w", "-I", str(d), str(d / "harness.cpp"), str(ROOT / "port" / "src" / "port_yz2.cpp"), "-o", str(exe)]
        r = subprocess.run(cc, capture_output=True, text=True)
        if r.returncode != 0:
            print("C harness build failed:\n" + r.stderr)
            return 1
        for i, s in enumerate(samples):
            for kind, arc in (("packed", encode_archive(s)), ("raw", raw_archive(s))):
                (d / "in.das").write_bytes(arc)
                subprocess.run([str(exe), str(d / "in.das"), str(d / "out.bin")], check=True)
                got = (d / "out.bin").read_bytes()
                assert got == s, f"C decoder: sample {i} ({kind}) differs ({len(got)} vs {len(s)} bytes): {got[:16]!r} vs {s[:16]!r}"
        print("C decoder (port/src/port_yz2.cpp) agrees on every sample")
    print("selftest ok")
    return 0


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "selftest":
        sys.exit(selftest())
    src, dst = Path(sys.argv[2]), Path(sys.argv[3])
    if cmd == "decode":
        dst.write_bytes(decode_archive(src.read_bytes()))
    elif cmd == "encode":
        dst.write_bytes(encode_archive(src.read_bytes()))
    elif cmd == "raw":
        dst.write_bytes(raw_archive(src.read_bytes()))
    else:
        sys.exit(__doc__)
    print(f"{src} -> {dst} ({dst.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
