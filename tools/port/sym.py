#!/usr/bin/env python3
"""Map PSP addresses to the port's functions: sym.py [--nm build/port/re4.nm] ADDR... (hex).
With no addresses, reads a PPSSPP log on stdin and annotates every 'PC xxxxxxxx' / 'LR xxxxxxxx'."""
import bisect, re, sys, subprocess

def load(nm):
    syms = []
    for line in open(nm):
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            syms.append((int(parts[0], 16), parts[2]))
    syms.sort()
    return [a for a, _ in syms], syms

def lookup(addrs, syms, a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return "?"
    base, name = syms[i]
    try:
        name = subprocess.run(["c++filt", name], capture_output=True, text=True).stdout.strip() or name
    except OSError:
        pass
    return "%s+0x%x" % (name, a - base)

def main():
    args = sys.argv[1:]
    nm = "build/port/re4.nm"
    if args[:1] == ["--nm"]:
        nm = args[1]; args = args[2:]
    addrs, syms = load(nm)
    if args:
        for a in args:
            print("%08x %s" % (int(a, 16), lookup(addrs, syms, int(a, 16))))
    else:
        for line in sys.stdin:
            line = line.rstrip("\n")
            for m in re.findall(r"\b(?:PC|LR) ([0-9a-f]{8})", line):
                line += "  [%s]" % lookup(addrs, syms, int(m, 16))
            print(line)

main()
