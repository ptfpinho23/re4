#!/usr/bin/env python3
"""Generate weak stubs for the SDK / runtime symbols the port's game objects still need.

The GameCube build linked the game against the Nintendo SDK (GX, OS, DVD, AX, ...), CRI and SN's
runtime (src/lib). The port replaces them with its platform layer under port/src; until every
entry point is implemented, this generates a weak definition per missing symbol so the game links
and reports what it calls. A strong definition in port/src overrides its stub without touching
this file.

    python3 tools/port/gen_stubs.py --objects build/port/CMakeFiles/re4_game.dir --out build/port/sdk_stubs.cpp

The undefined symbols come from `psp-nm` over the objects, minus what the objects define and minus
what the PSP toolchain's libraries define (libc, libm, libgcc, libstdc++, the SDK). Each remaining
C symbol is looked up as a prototype (or `extern` data declaration) in include/ and the header that
declares it is included by the generated file. Symbols with no declaration are listed in the
output as a comment and must be added by hand (port/src).
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = ROOT / "include"


def nm(tool, args):
    return subprocess.run([tool, *args], capture_output=True, text=True, errors="replace").stdout


def objects_under(d: Path):
    return sorted(str(p) for p in d.rglob("*.obj")) + sorted(str(p) for p in d.rglob("*.o"))


def symbols(tool, objs, flag):
    out = nm(tool, [flag, *objs])
    names = set()
    for line in out.split("\n"):
        parts = line.split()
        if not parts or line.endswith(":"):
            continue
        names.add(parts[-1])
    return names


def toolchain_defined(tool, pspdev: Path):
    libs = []
    for pat in ("psp/lib/libc.a", "psp/lib/libm.a", "psp/lib/libstdc++.a", "psp/lib/libpspvram.a",
                "psp/lib/libpsp*.a"):
        libs += [str(p) for p in pspdev.glob(pat)]
    libs += [str(p) for p in pspdev.glob("lib/gcc/psp/*/libgcc.a")]
    libs += [str(p) for p in (pspdev / "psp" / "sdk" / "lib").glob("*.a")]
    names = set()
    for lib in libs:
        out = nm(tool, ["--defined-only", lib])
        for line in out.split("\n"):
            parts = line.split()
            if len(parts) == 3:
                names.add(parts[2])
    return names


COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)


def header_texts():
    texts = {}
    for p in sorted(INCLUDE.rglob("*.h")):
        if "prodg" in p.parts or "libc" in p.parts:
            continue
        texts[p] = COMMENT.sub(lambda m: " " * len(m.group(0)), p.read_text(errors="replace"))
    return texts


def find_decl(name, texts):
    """Returns (kind, header, decl_text). kind: 'func' or 'data'."""
    func = re.compile(r"(?m)^[ \t]*((?:extern\s+\"C\"\s+)?(?:extern\s+)?[A-Za-z_][\w \t\*]*?\**)[ \t]*\b" + re.escape(name)
                      + r"[ \t]*\(([^;{}]*?)\)[ \t]*;")
    data = re.compile(r"(?m)^[ \t]*extern\s+((?:\"C\"\s+)?[A-Za-z_][\w \t\*]*?\**)[ \t]*\b" + re.escape(name) + r"[ \t]*(\[[^;]*\])?[ \t]*;")
    # game-side headers first (they are the declarations the units compiled against), dolphin after
    # top-level game headers first, then the game subdirectories, dolphin last
    order = sorted(texts, key=lambda p: ("dolphin" in p.parts, len(p.relative_to(INCLUDE).parts), str(p)))
    for p in order:
        t = texts[p]
        m = func.search(t)
        if m:
            ret = re.sub(r'extern\s+"C"\s+|extern\s+|static\s+|inline\s+', "", m.group(1)).strip()
            if ret in ("", "return", "else"):
                continue
            return "func", p, ret, m.group(2).strip()
        m = data.search(t)
        if m:
            typ = re.sub(r'"C"\s+', "", m.group(1)).strip()
            return "data", p, typ, m.group(2) or ""
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objects", required=True, type=Path, action="append")
    ap.add_argument("--defined", type=Path, action="append", default=[],
                    help="more object directories whose definitions count (the platform layer)")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--pspdev", type=Path, default=Path(os.environ.get("PSPDEV", "")))
    ap.add_argument("--nm", default="psp-nm")
    args = ap.parse_args()

    objs = [o for d in args.objects for o in objects_under(d)]
    if not objs:
        sys.exit(f"no objects under {args.objects}")
    defined = symbols(args.nm, objs, "--defined-only")
    for d in args.defined:
        more = objects_under(d)
        if more:
            defined |= symbols(args.nm, more, "--defined-only")
    undefined = symbols(args.nm, objs, "-u") - defined
    provided = toolchain_defined(args.nm, args.pspdev) if args.pspdev.exists() else set()
    missing = sorted(s for s in undefined if s not in provided and not s.startswith("_Z"))
    cxx_missing = sorted(s for s in undefined if s not in provided and s.startswith("_Z"))

    texts = header_texts()
    headers = []
    stubs = []
    stub_headers = []
    data_syms = []
    unknown = []
    for name in missing:
        d = find_decl(name, texts)
        if not d:
            unknown.append(name)
            continue
        kind, hdr, typ, rest = d
        rel = hdr.relative_to(INCLUDE).as_posix()
        if rel not in headers:
            headers.append(rel)
        stub_headers.append(rel)
        if kind == "func":
            params = rest if rest not in ("", "void") else "void"
            body = "" if typ == "void" else f" return port_zero<{typ}>();"
            stubs.append(f'{typ} __attribute__((weak)) {name}({params}) {{ PORT_STUB("{name}");{body} }}')
        else:
            headers.pop() if headers and headers[-1] == rel and stub_headers.count(rel) == 1 else None
            stub_headers.pop()
            data_syms.append(f"{typ} {name}{rest};  // {rel}")

    def emit(path, hdrs, stub_lines, trailer):
        lines = [
            "// Generated by tools/port/gen_stubs.py: weak stubs for the SDK / runtime symbols the game",
            "// objects reference and port/src does not yet define. A strong definition in port/src wins.",
            '#include "port.h"',
            '#include "port_stub.h"',
            "#include <string.h>",
        ]
        lines += [f'#include "{h}"' for h in hdrs]
        lines += ["", "template <class R> static R port_zero() { R r; memset((void*) &r, 0, sizeof(r)); return r; }", "",
                  'extern "C" {'] + stub_lines + ["}", ""] + trailer
        path.write_text("\n".join(lines) + "\n")

    # The game-side headers (include/*.h) and the SDK's own (include/dolphin/) declare the same
    # functions with different parameter types; a stub file includes one family only.
    game_hdrs = [h for h in headers if not h.startswith("dolphin/")]
    dol_hdrs = [h for h in headers if h.startswith("dolphin/")]
    game_stubs = [s for s, h in zip(stubs, stub_headers) if not h.startswith("dolphin/")]
    dol_stubs = [s for s, h in zip(stubs, stub_headers) if h.startswith("dolphin/")]
    trailer = []
    if data_syms:
        trailer.append("// Data symbols (define them in port/src with the header's linkage):")
        trailer += [f"//   {d}" for d in data_syms]
    if unknown:
        trailer.append("// No declaration found in include/ for these (define them in port/src):")
        trailer += [f"//   {n}" for n in unknown]
    if cxx_missing:
        trailer.append("// C++ symbols still undefined (defined in a unit the port drops, or an inline the old compiler emitted):")
        trailer += [f"//   {n}" for n in cxx_missing]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    emit(args.out, game_hdrs, game_stubs, trailer)
    emit(args.out.with_name(args.out.stem + "_dolphin" + args.out.suffix), dol_hdrs, dol_stubs, [])
    print(f"{len(stubs)} stubs, {len(data_syms)} data, {len(unknown)} undeclared, {len(cxx_missing)} C++ undefined -> {args.out}")
    for d in data_syms:
        print("  data:", d)
    for n in unknown:
        print("  undeclared:", n)
    for n in cxx_missing:
        print("  C++:", n)


if __name__ == "__main__":
    main()
