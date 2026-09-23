# RE4 PSP port

The second build target of this repository: the game sources of the matching tree compiled with
the PSP toolchain against a platform layer written for the PSP, instead of the Nintendo SDK, CRI
and SN runtime that `src/lib` reproduces. The matching build (`configure.py` + `ninja`, byte-
identical to the GameCube discs) is not touched: every accommodation the port needs in `src/` and
`include/` is behind `RE4_PORT`, expands to the original tokens when it is not defined, and
`tools/port/linecheck.py` proves no `__LINE__` value moved (the assert strings are part of the
bytes).

## Setup (macOS)

```sh
# toolchain: pspdev release (psp-gcc 15, newlib, pspsdk), plus the libraries its GCC dlopens
curl -L https://github.com/pspdev/pspdev/releases/latest/download/pspdev-macos-latest-arm64.tar.gz | tar xz -C ~/pspdev
brew install gmp libmpc mpfr isl zstd cmake ninja
brew install --cask ppsspp           # the emulator (PPSSPPSDL.app)
export PSPDEV=$HOME/pspdev/pspdev
export PATH=$PSPDEV/bin:$PATH
```

## Build and run

```sh
cmake -S port -B build/port -G Ninja   # picks $PSPDEV/psp/share/pspdev.cmake
ninja -C build/port compile_all        # milestone 1: every game unit compiles (661 objects)
ninja -C build/port re4                # milestone 2: build/port/EBOOT.PBP (the DOL units + platform layer)
port/run-ppsspp.sh 25                  # boots the EBOOT in PPSSPP for 25 s and prints the game's stdout
PPSSPP_FLAGS=-d port/run-ppsspp.sh 25  # with PPSSPP's debug log (HLE calls)
```

`ninja re4` regenerates `build/port/sdk_stubs*.cpp` (tools/port/gen_stubs.py): one weak stub per
SDK / runtime symbol the game objects reference and `port/src` does not define, each reporting
itself once as `[port] unimplemented: <name>` when called. Implement an entry point by giving it a
strong definition under `port/src`; the stub disappears from the next generation.

PPSSPP notes: `FastMemoryAccess = False` in `~/.config/ppsspp/PSP/SYSTEM/ppsspp.ini` (macOS has no
host exception handler, so a bad guest access would crash the emulator instead of being reported);
the run script clears `FailedGraphicsBackends.txt` (written when PPSSPP is killed) and quits the app
through an Apple event.

## Layout

- `CMakeLists.txt` — the target: `re4_game` (DOL units), `re4_mod_<name>` (one per REL module,
  compile-only for now), `re4_port` (platform layer), `re4` (EBOOT). Unit lists come from
  `config/G4BE08/{objects,modules}.py` through `tools/port/units.py`.
- `include/` — port-only headers (`port_stub.h`). `include/port.h` in the main tree carries the
  macros both builds see (`REG_PIN`, `ASM_ANCHOR`, `UNIT_INLINE`, `LC_BASE`).
- `src/` — the platform layer: `port_log.cpp` (OSReport, stub reports on stdout), `port_runtime.cpp`
  (GCC 2.95 allocator names, memclr/memset, yz2 and CRI placeholders), `port_data.cpp` (SDK data
  symbols), `port_mem.cpp` (the locked cache as RAM).
- `app/psp_main.cpp` — the PSP module header; the game's `main()` is the entry point.
- `run-ppsspp.sh` — boot the EBOOT in the emulator and print its output.

## What the port changed in the matching tree (all behind RE4_PORT)

- `include/port.h`, included from `include/types.h` and `include/dolphin/types.h`.
- 130 register pins `asm("rN")` -> `REG_PIN("rN")`, 143 section anchors / `.comm` statements ->
  `ASM_ANCHOR(...)` (same line, same tokens for the matching build).
- The PowerPC kernels in C: `math_sub.cpp` (SQRTF, SINF, COSF, LIMIT_ANGLE), `trans.cpp` (the
  `CalcSk1_x` skinning kernels, GQR6 scale decoding, `PSQ_L_U8`), `shape.cpp` / `dbmodule.cpp` /
  `Espgen42.cpp` / `espgen45.cpp` (`PSQ_*` macros), `main.cpp` / `scheduler.cpp` (GQR setup skipped).
- `cAtariInfo` has no constructor in the port (GCC 15 rejects it inside cModel's anonymous union);
  `ATARI_INFO_CONSTRUCT` in `model.cpp`. `fabsf` comes from libm (`math_sub.h`, `dolphin/types.h`).
- `dbg_var.h` names its dependent base members; three `inline` members other units call are
  `UNIT_INLINE`; `cManager<cObj>::destroyNow` is instantiated explicitly in `objRocket.cpp`.
- The asm symbol-label aliases (`extern T x asm("y")`, `f() asm("mangled")`) get a macro or a
  small function in the port branch (`emshield`, `route_ck`, `card`, `main_mem`, `obj02`/`r318`,
  seven `EvtFlgOnStatus` rooms, the tool modules).

## Game data on the memory stick

The port boots from the memory stick like any homebrew and reads the game's files from a
directory next to the EBOOT:

    ms0:/PSP/GAME/RE4/EBOOT.PBP
    ms0:/PSP/GAME/RE4/data/bgm/bio4str.hed, data/etc/core.das, data/st1/r100.das, ...

`data/` is the disc's own tree, both discs merged (disc 2 only adds the island stage). Fill it from
the GameCube images with `tools/port/gamedata.py extract <disc1.iso> build/port/data` and then
`... extract <disc2.gcm> build/port/data`, and copy `build/port/EBOOT.PBP` plus `build/port/data`
to `ms0:/PSP/GAME/RE4/`. `port/run-ppsspp.sh` installs the same layout into PPSSPP's memory stick
folder (`~/.config/ppsspp/PSP/GAME/RE4/`, `data` as a symlink to `build/port/data`) and boots it,
so the emulator sees exactly what the PSP will.

The files stay as the disc has them. Big-endian data is handled at run time, not by converters:
the file-resident structures the engine reads in place get the byte-swapping field types of
`include/port_be.h` (`be_u32`, `be_u16`, `be_f32`...; plain typedefs in the matching build), the
loaders' offset-to-pointer relocations read the stored offsets through `FILE_U32` and test
"already relocated" through `IS_RELOCATED`, mesh data and textures are read big-endian by the GX
layer itself, and the yz2 archives (`.das`) are decoded by `port/src/port_yz2.cpp`, a C port of
the assembly decoder (`tools/port/yz2.py` is the same codec in Python with an encoder; its
self-test round-trips both). `gamedata.py` can also unpack archives to raw form (`yz2.py raw`) to
skip the decode on the PSP. Converting the structures is done for the core archive table, the
texture palettes and the sound tables; the rest (models' headers, motion, rooms, effects, items,
messages...) follows the same pattern as each loader is reached with real data.

Everything the port logs (and the first 120 lines of the game's own output) is also drawn on the
screen, since a PSP has no stdout; HOME exits through the usual exit callback. Memory: the port
keeps the GameCube's 21 MB arena and an 8 MB ARAM buffer as static arrays, so it needs a
PSP-2000 or later (large memory mode, MEMSIZE=1 in the PARAM.SFO); a PSP-1000's 24 MB will not
hold it until the map is trimmed.

## State (2026-09-23)

All 661 game units compile; the 293 DOL units and all 105 game modules (the 9 debug tool
modules stay out) link into one EBOOT (9.2 MB of code) that boots from the memory-stick layout.
The platform layer carries the game through its whole boot: memory map and heaps (the SDK's
OSAlloc over a 21 MB arena, `GC_ADDR`), OS time / interrupt / misc services, threads (the
scheduler's task threads on PSP threads, the retrace callback on a vblank thread), the matrix
library (the SDK's C functions), ARAM as RAM with an immediate DMA queue, the pad, the `data/`
tree as the disc, the REL modules statically linked behind `OSLink` (tools/port/modlink.py:
partial links with the entry points renamed, cross-module imports bound to the real definition
or the module's own trap, everything else localized), and a first renderer, `port_gx.cpp`: GX on
the GE with the vertex assembly on the CPU (immediate mode and disc display lists, big-endian
mesh data read as-is), GameCube texture formats converted on first use (CMPR to DXT1), stage-0
TEV, blend / depth / cull / alpha / fog state. With two zero-filled placeholder files the game
runs its main loop with the GE live and no faults.

Fixed on the way: five functions that fall off their end (GCC 2.95 returned the last call's
value; GCC 15 makes that unreachable code and the game spun), three DOL statics the modules
import by name, two anchor-defined constants.

Not yet: hardware lighting, multi-stage TEV / indirect textures, framebuffer copies (the post
filters), sound (AX / MIX / SYN / SEQ are stubs), saves (CARD), the CRI movie player, and the
big-endian field types on most file-resident structures (only the core archive table, the
texture palettes and the sound tables carry them so far); checking each loader needs the
extracted files, which are not in the repository.
