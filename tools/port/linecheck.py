#!/usr/bin/env python3
"""Byte-safety check for port edits: __LINE__ / __FILE__ values must not move.

The matching build reproduces the vendor's assert strings ("D:/Bio4/Prog/x.cpp", line) through
`#line` directives, but not every site that expands __LINE__ has its own `#line` right in front
of it: a line inserted between a `#line` directive and a later expansion site shifts the number
that lands in .rodata and changes the bytes. Port edits (`#ifdef RE4_PORT` blocks, macro
wrappers) therefore have to keep every site's effective (file, line) pair unchanged.

This tool computes, for every source and header, the effective __FILE__/__LINE__ at every
expansion site (macros that expand __LINE__: MEM_ALLOC, MEM_CALLOC, HALT, dbgAssert, ASSERT*,
VECNormalize, and literal __LINE__), both in the working tree and in a git revision (HEAD by
default), and reports every site whose value moved or whose count changed.

    python3 tools/port/linecheck.py            # working tree vs HEAD
    python3 tools/port/linecheck.py --rev abc  # vs another revision
    python3 tools/port/linecheck.py --dump     # print the site table of the working tree

Exit status 1 when a site moved. Header sites matter as well (atari.h, widget.h, light.h ... expand
__LINE__ inside inline functions), so headers are checked too.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SITE = re.compile(
    r"\b(MEM_ALLOC|MEM_CALLOC|HALT|dbgAssert|ASSERT|ASSERTLINE|ASSERTMSGLINE|ASSERTMSGLINEV|assert|VECNormalize)\s*\("
    r"|__LINE__"
)
LINE_DIRECTIVE = re.compile(r'^\s*#\s*line\s+(\d+)(?:\s+"([^"]*)")?')
GNU_LINE_DIRECTIVE = re.compile(r'^\s*#\s+(\d+)\s+"([^"]*)"')


def sites(text: str, path: str):
    """(site text, effective file, effective line) for every expansion site in `text`."""
    out = []
    cur_file = path
    cur_line = 1
    for raw in text.split("\n"):
        m = LINE_DIRECTIVE.match(raw) or GNU_LINE_DIRECTIVE.match(raw)
        if m:
            cur_line = int(m.group(1))
            if m.group(2) is not None:
                cur_file = m.group(2)
            continue
        stripped = raw.lstrip()
        if not stripped.startswith("//") and not stripped.startswith("#define"):
            for s in SITE.finditer(raw):
                out.append((s.group(0).replace(" ", ""), cur_file, cur_line))
        cur_line += 1
    return out


def tracked_files(rev: str):
    ls = subprocess.run(["git", "ls-tree", "-r", "--name-only", rev, "src", "include"],
                        cwd=ROOT, capture_output=True, text=True, check=True).stdout.split("\n")
    return [f for f in ls if f.endswith((".c", ".cpp", ".h"))]


def read_rev(rev: str, path: str) -> str:
    return subprocess.run(["git", "show", f"{rev}:{path}"], cwd=ROOT, capture_output=True,
                          text=True, errors="replace", check=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rev", default="HEAD")
    ap.add_argument("--dump", action="store_true")
    args = ap.parse_args()

    if args.dump:
        for f in sorted(tracked_files("HEAD")):
            p = ROOT / f
            if p.exists():
                for s in sites(p.read_text(errors="replace"), f):
                    print(f"{f}\t{s[0]}\t{s[1]}\t{s[2]}")
        return 0

    changed = subprocess.run(["git", "diff", "--name-only", args.rev, "--", "src", "include"],
                             cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    untracked = subprocess.run(["git", "ls-files", "--others", "--exclude-standard", "src", "include"],
                               cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    bad = 0
    for f in sorted(set(changed)):
        if not f.endswith((".c", ".cpp", ".h")):
            continue
        p = ROOT / f
        new = sites(p.read_text(errors="replace"), f) if p.exists() else []
        try:
            old = sites(read_rev(args.rev, f), f)
        except subprocess.CalledProcessError:
            old = []  # new file: nothing to preserve
        if len(old) != len(new):
            print(f"{f}: site count {len(old)} -> {len(new)}")
            bad += 1
        for i, (a, b) in enumerate(zip(old, new)):
            if a != b:
                print(f"{f}: site {i} moved: {a} -> {b}")
                bad += 1
    if untracked:
        print(f"note: {len(untracked)} untracked file(s) under src/include (not checked)")
    if bad:
        print(f"FAIL: {bad} __LINE__ site(s) changed")
        return 1
    print(f"OK: {len(changed)} changed file(s), no __LINE__ site moved")
    return 0


if __name__ == "__main__":
    sys.exit(main())
