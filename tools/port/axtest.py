#!/usr/bin/env python3
"""Host test of the port's AX voice mixer (port/src/port_ax.cpp) against a Python reference.

Builds port/test/ax_host (the mixer driven by a command script, no audio thread), encodes test
samples as GameCube DSP-ADPCM here and checks the mixed frames: the nibble order and frame
headers, the predictor, one-shot ends (pb.state, the driver's u32 address view), loops with the
loop predictor state, the sample-rate converter (drop and linear modes), the MIX pan and volume.

    python3 tools/port/axtest.py
"""
import math
import random
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "build" / "port" / "ax_host"


def build():
    cmd = ["clang++", "-std=c++14", "-O1", "-w", "-DRE4_PORT=1", "-I", str(ROOT / "port/test/hostinc"), "-I", str(ROOT / "port/include"),
           "-I", str(ROOT / "include"), str(ROOT / "port/src/port_ax.cpp"), str(ROOT / "port/test/ax_host.cpp"), "-o", str(HOST)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("harness build failed:\n" + r.stderr)


def run(script):
    r = subprocess.run([str(HOST)], input=script, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("harness failed:\n" + r.stderr + r.stdout)
    return r.stdout.split("\n")


def check(cond, msg):
    if not cond:
        sys.exit("FAIL: " + (msg() if callable(msg) else msg))


def first_diff(a, b):
    return next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), None)


def clamp16(v):
    return max(-32768, min(32767, v))


def encode_adpcm(nibbles_per_frame, coefs, coef_index):
    """nibbles_per_frame: list of (scale, [14 nibbles in -8..7]). Returns (bytes, decoded samples)
    decoded with the given coefficient pair through the DSP-ADPCM predictor from yn1 = yn2 = 0."""
    data = bytearray()
    samples = []
    yn1 = yn2 = 0
    c1, c2 = coefs[coef_index]
    for scale, nibs in nibbles_per_frame:
        data.append((coef_index << 4) | scale)
        for i in range(0, 14, 2):
            data.append(((nibs[i] & 15) << 4) | (nibs[i + 1] & 15))
        for n in nibs:
            s = clamp16(((n * (1 << scale)) * 2048 + 1024 + c1 * yn1 + c2 * yn2) >> 11)
            yn2, yn1 = yn1, s
            samples.append(s)
    return bytes(data), samples


def frames(lines):
    out = []
    for l in lines:
        if l.startswith("F "):
            v = [int(x) for x in l.split()[1:]]
            out.append([(v[2 * i], v[2 * i + 1]) for i in range(160)])
    return out


def status(lines):
    return [tuple(int(x) for x in l.split()[1:]) for l in lines if l.startswith("STATUS ")]


def setup(rnd, nframes, coefs, coef_index, aram_ofs=0x100):
    nibs = [(rnd.randrange(1, 6), [rnd.randrange(-8, 8) for _ in range(14)]) for _ in range(nframes)]
    data, samples = encode_adpcm(nibs, coefs, coef_index)
    coef_args = " ".join(str(c & 0xFFFF) for pair in coefs for c in pair)
    script = f"aram {aram_ofs:x} {data.hex()}\nvoice 15\n"
    script += f"adpcm 0 {coef_args} {(coef_index << 4) | nibs[0][0]} 0 0\n"
    return script, samples


def test_oneshot(rnd):
    coefs = [(0, 0)] * 8
    script, samples = setup(rnd, 20, coefs, 0)          # 280 samples
    cur = 0x100 * 2 + 2
    end = cur + (len(samples) // 14) * 16 + len(samples) % 14
    script += f"addr 0 0 0 {0x4000 * 2 + 2} {end} {cur}\nratio 0 65536\nsrctype 0 0\nmix 0 0 -904 -904 0 127 0\nstate 0 1\n"
    script += "frame\nstatus 0\nframe\nstatus 0\n"
    lines = run(script)
    f = frames(lines)
    got = [s[0] for s in f[0]] + [s[0] for s in f[1]]
    exp = samples + [0] * (320 - len(samples))
    check(got == exp, lambda: f"one-shot samples differ at {first_diff(got, exp)}: {got[:8]} vs {exp[:8]}")
    check(all(s[1] == 0 for s in f[0]), "pan 0 leaks into the right channel")
    st = status(lines)
    # the mixer fetches one sample ahead for its interpolation, so the address may lead by one
    pos = cur + 160 // 14 * 16 + 160 % 14
    check(st[0][0] == 1 and pos <= st[0][1] <= pos + 3, f"address after one frame {st[0]} vs {pos}")
    check(st[1] == (0, end), f"stopped one-shot {st[1]} vs (0, {end})")
    print("  one-shot ADPCM (nibble order, frame headers, end, u32 address view) ok")


def test_predictor(rnd):
    coefs = [(0, 0), (2048, 0), (0, -1024), (1536, -512)] + [(0, 0)] * 4
    for ci in (1, 2, 3):
        script, samples = setup(rnd, 12, coefs, ci)     # 168 samples
        cur = 0x100 * 2 + 2
        end = cur + 12 * 16
        script += f"addr 0 0 0 {0x4000 * 2 + 2} {end} {cur}\nratio 0 65536\nsrctype 0 0\nmix 0 0 -904 -904 0 127 0\nstate 0 1\nframe\nframe\n"
        f = frames(run(script))
        got = [s[0] for s in f[0]] + [s[0] for s in f[1]]
        exp = samples + [0] * (320 - len(samples))
        check(got == exp, lambda: f"predictor coef {ci}: differ at {first_diff(got, exp)}")
    print("  ADPCM predictor (three coefficient pairs, clamping) ok")


def test_loop(rnd):
    coefs = [(2048, 0)] + [(0, 0)] * 7
    script, samples = setup(rnd, 4, coefs, 0)           # 56 samples, looped whole
    cur = 0x100 * 2 + 2
    end = cur + 4 * 16
    lps = (0 << 4) | 3
    # the loop restarts at the first sample with the loop predictor state (pred/scale, yn1, yn2)
    script += f"adpcmloop 0 {lps} 100 -50\naddr 0 1 0 {cur} {end} {cur}\nratio 0 65536\nsrctype 0 0\nmix 0 0 -904 -904 0 127 0\nstate 0 1\nframe\nframe\nstatus 0\n"
    lines = run(script)
    f = frames(lines)
    got = [s[0] for s in f[0]] + [s[0] for s in f[1]]
    # reference: first pass from zero state, later passes from the loop state with scale 3 on frame 0
    data = bytes.fromhex(script.split("\n")[0].split()[2])
    def decode_pass(yn1, yn2, first_scale):
        out = []
        for fr in range(4):
            scale = data[fr * 8] & 15 if fr else first_scale
            for i in range(14):
                b = data[fr * 8 + 1 + i // 2]
                n = (b >> 4) if i % 2 == 0 else (b & 15)
                if n > 7: n -= 16
                s = clamp16(((n * (1 << scale)) * 2048 + 1024 + 2048 * yn1) >> 11)
                yn2, yn1 = yn1, s
                out.append(s)
        return out, yn1, yn2
    exp, y1, y2 = decode_pass(0, 0, data[0] & 15)
    while len(exp) < 320:
        more, y1, y2 = decode_pass(100, -50, 3)
        exp += more
    exp = exp[:320]
    check(got == exp, lambda: f"loop: differ at {first_diff(got, exp)}: {got[50:60]} vs {exp[50:60]}")
    st = status(lines)
    check(st[0][0] == 1, "looping voice stopped")
    print("  looping voice with the loop predictor state ok")


def test_src_and_mix(rnd):
    coefs = [(0, 0)] * 8
    script, samples = setup(rnd, 40, coefs, 0)          # 560 samples
    cur = 0x100 * 2 + 2
    end = cur + 40 * 16
    base = script
    # ratio 2.0, drop mode: every other sample
    s = base + f"addr 0 0 0 {0x4000 * 2 + 2} {end} {cur}\nratio 0 {2 << 16}\nsrctype 0 0\nmix 0 0 -904 -904 0 127 0\nstate 0 1\nframe\n"
    got = [x[0] for x in frames(run(s))[0]]
    check(got == samples[0:320:2], "ratio 2.0 drop")
    # ratio 0.5, linear: midpoints between consecutive samples
    s = base + f"addr 0 0 0 {0x4000 * 2 + 2} {end} {cur}\nratio 0 {1 << 15}\nsrctype 0 1\nmix 0 0 -904 -904 0 127 0\nstate 0 1\nframe\n"
    got = [x[0] for x in frames(run(s))[0]]
    exp = []
    for i in range(160):
        j = i // 2
        if i % 2 == 0:
            exp.append(samples[j])
        else:
            exp.append(samples[j] + (((samples[j + 1] - samples[j]) * 32768) >> 16))
    check(got == exp, lambda: f"ratio 0.5 linear: differ at {first_diff(got, exp)}: {got[:6]} vs {exp[:6]}")
    # pan 64 (centre): both channels at cos(45 deg); input -60 (-6 dB) halves the amplitude
    s = base + f"addr 0 0 0 {0x4000 * 2 + 2} {end} {cur}\nratio 0 65536\nsrctype 0 0\nmix 0 -60 -904 -904 64 127 0\nstate 0 1\nframe\n"
    got = frames(run(s))[0]
    gl = int(math.cos(64 / 127 * math.pi / 2) * 10 ** (-60 / 200) * 4096)
    gr = int(math.sin(64 / 127 * math.pi / 2) * 10 ** (-60 / 200) * 4096)
    for i in range(160):
        el, er = (samples[i] * gl) >> 12, (samples[i] * gr) >> 12
        check(abs(got[i][0] - el) <= 1 and abs(got[i][1] - er) <= 1, f"mix at {i}: {got[i]} vs {(el, er)}")
    # silence at -904, two voices summed
    s = base + f"addr 0 0 0 {0x4000 * 2 + 2} {end} {cur}\nratio 0 65536\nsrctype 0 0\nmix 0 -904 -904 -904 0 127 0\nstate 0 1\n"
    s += f"voice 15\nadpcm 1 {' '.join('0' for _ in range(16))} {(0 << 4) | (int(base.split(chr(10))[0].split()[2][:2], 16) & 15)} 0 0\naddr 1 0 0 {0x4000 * 2 + 2} {end} {cur}\nratio 1 65536\nsrctype 1 0\nmix 1 0 -904 -904 127 127 0\nstate 1 1\nframe\n"
    got = frames(run(s))[0]
    check(all(x[0] == 0 for x in got) and [x[1] for x in got] == samples[:160], "silent voice / second voice panned right")
    print("  sample-rate converter (drop, linear), MIX pan / volume, voice sum ok")


def main():
    build()
    rnd = random.Random(7)
    test_oneshot(rnd)
    test_predictor(rnd)
    test_loop(rnd)
    test_src_and_mix(rnd)
    print("axtest ok")


if __name__ == "__main__":
    main()
