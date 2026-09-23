#!/usr/bin/env python3
"""Link the REL modules statically into the port.

On the GameCube every module (rooms, enemies, weapons, the sub screen, the tools) is a REL file
the game loads from disc and links with OSLink; imports the loaded set does not provide bind to
the module's own `_unresolved` trap (a stage module registers every room of its stage, the ones
in sibling modules included). The port keeps all modules in the executable instead:

  1. each module's objects (compiled with _prolog / _epilog / _unresolved renamed to
     re4_<mod>_prolog ...) are partially linked; its .init_array, .data and .bss go into named
     sections with start / end symbols so the port can run the constructors through `_ctors` and
     reset the data on every link, as a fresh REL load would;
  2. symbols a module needs that no game / platform object provides but another module defines
     are bound to that module's re4_<mod>_unresolved;
  3. every other symbol is made local, so shared sources compiled into several modules and the
     module-private globals never clash;
  4. a registry (port_modules_gen.cpp) maps the REL module ids (config/G4BE08/modules/*/rel.json)
     to the entry points and section ranges for port/src/port_modules.cpp.

    python3 tools/port/modlink.py --build build/port --out build/port/mods
"""
import argparse
import glob
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{' '.join(cmd[:3])} ...: {r.stderr.strip()}")
    return r.stdout


def nm(tool, objs, flag):
    names = set()
    for line in run([tool, flag, *objs]).split("\n"):
        p = line.split()
        if p and not line.endswith(":"):
            names.add(p[-1])
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--prefix", default="psp-")
    a = ap.parse_args()
    nm_tool, ld, objcopy = a.prefix + "nm", a.prefix + "ld", a.prefix + "objcopy"
    a.out.mkdir(parents=True, exist_ok=True)
    for stale in a.out.glob("*.o"):
        stale.unlink()

    game_objs = glob.glob(str(a.build / "CMakeFiles" / "re4_game.dir" / "**" / "*.obj"), recursive=True)
    port_objs = glob.glob(str(a.build / "CMakeFiles" / "re4_port.dir" / "**" / "*.obj"), recursive=True)
    provided = nm(nm_tool, game_objs + port_objs, "--defined-only")

    mods = {}
    for d in sorted(glob.glob(str(a.build / "CMakeFiles" / "re4_mod_*.dir"))):
        name = os.path.basename(d)[len("re4_mod_"):-len(".dir")]
        if name == "Tools" or name.startswith("t_"):
            continue  # the developers' debug editors: host file server, SN tools; not part of the port
        objs = glob.glob(d + "/**/*.obj", recursive=True)
        if objs:
            mods[name] = objs

    # pass 1: partial links with the renamed sections
    tmp = {}
    for name, objs in mods.items():
        script = a.out / f"{name}.ld"
        script.write_text(f"""SECTIONS {{
  .re4init_{name} : {{ re4_{name}_ctors_start = .; KEEP(*(.init_array*)) KEEP(*(.ctors*)) LONG(0); re4_{name}_dtors_start = .; LONG(0); }}
  .re4data_{name} : {{ re4_{name}_data_start = .; *(.data*) re4_{name}_data_end = .; }}
  .re4bss_{name} : {{ re4_{name}_bss_start = .; *(.bss*) *(COMMON) re4_{name}_bss_end = .; }}
}}
""")
        out1 = a.out / f"{name}.1.o"
        run([ld, "-r", "--allow-multiple-definition", "-T", str(script), f"--defsym=_ctors=re4_{name}_ctors_start", f"--defsym=_dtors=re4_{name}_dtors_start",
             *objs, "-o", str(out1)])
        tmp[name] = out1

    # pass 2: what every module defines. A symbol another module needs that exactly one module
    # defines (an enemy module's methods a room calls) stays global there and binds to it; one
    # that several modules define (sibling stage rooms, shared sources) binds to the trap.
    defined = {name: nm(nm_tool, [str(o)], "--defined-only") for name, o in tmp.items()}
    owners = {}
    for name, d in defined.items():
        for s in d:
            owners.setdefault(s, []).append(name)
    undefined = {name: nm(nm_tool, [str(o)], "-u") for name, o in tmp.items()}
    keep = {name: set() for name in tmp}
    trap = {name: [] for name in tmp}
    for name in tmp:
        for s in undefined[name]:
            if s in provided or s in defined[name] or s not in owners:
                continue
            if len(owners[s]) == 1:
                keep[owners[s][0]].add(s)
            else:
                trap[name].append(s)
    registry = []
    for name, out1 in tmp.items():
        out2 = a.out / f"{name}.2.o"
        run([ld, "-r", *[f"--defsym={s}=re4_{name}_unresolved" for s in sorted(trap[name])], str(out1), "-o", str(out2)])
        final = a.out / f"{name}.mod.o"
        # -R .group: the COMDAT groups of inline functions, templates and vtables would be merged
        # across modules by the final link and all but one copy dropped; without their group
        # sections every module keeps its own (now local) copy.
        run([objcopy, "-w", "-R", ".group", "-G", f"re4_{name}_*", *[f"--keep-global-symbol={s}" for s in sorted(keep[name])], str(out2), str(final)])
        rel = json.load(open(ROOT / "config" / "G4BE08" / "modules" / name / "rel.json"))
        registry.append((rel["module_id"], name, len(trap[name])))
        os.unlink(out1)
        os.unlink(out2)
    exported = sum(len(k) for k in keep.values())

    registry.sort()
    lines = ["// Generated by tools/port/modlink.py: the REL modules linked into the executable.",
             '#include "port_modules.h"', ""]
    for mid, name, _ in registry:
        lines.append(f'extern "C" void re4_{name}_prolog(void); extern "C" void re4_{name}_epilog(void); extern "C" void re4_{name}_unresolved(void);')
        lines.append(f"extern unsigned char re4_{name}_data_start[], re4_{name}_data_end[], re4_{name}_bss_start[], re4_{name}_bss_end[];")
    lines += ["", "const PortModule port_modules[] = {"]
    for mid, name, _ in registry:
        lines.append(f'    {{{mid}, "{name}", re4_{name}_prolog, re4_{name}_epilog, re4_{name}_unresolved, '
                     f"re4_{name}_data_start, re4_{name}_data_end, re4_{name}_bss_start, re4_{name}_bss_end, 0}},")
    lines += ["};", f"const int port_module_count = {len(registry)};", ""]
    (a.out / "port_modules_gen.cpp").write_text("\n".join(lines))
    print(f"{len(registry)} modules -> {a.out} ({sum(m for _, _, m in registry)} cross-module references bound to the traps, {exported} symbols exported between modules)")


if __name__ == "__main__":
    main()
