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
to `ms0:/PSP/GAME/RE4/`. The extractor is where the asset converters plug in (`CONVERTERS`, per
path pattern); there are none yet, so the tree still holds big-endian game data and the engine
misreads it. `port/run-ppsspp.sh` installs the same layout into PPSSPP's memory stick folder
(`~/.config/ppsspp/PSP/GAME/RE4/`, `data` as a symlink to `build/port/data`) and boots it, so the
emulator sees exactly what the PSP will.

Everything the port logs (and the first 120 lines of the game's own output) is also drawn on the
screen through the pspsdk debug console, since a PSP has no stdout; HOME exits through the usual
exit callback. Memory: the port keeps the GameCube's 21 MB arena and an 8 MB ARAM buffer as
static arrays, so it needs a PSP-2000 or later (large memory mode, MEMSIZE=1 in the PARAM.SFO);
a PSP-1000's 24 MB will not hold it until the map is trimmed.

## State (2026-09-22, night)

All 661 game units compile; the 293 DOL units link into an EBOOT that boots in PPSSPP from the
memory-stick layout. The platform layer carries the game through its whole boot: memory map and
heaps (the SDK's OSAlloc over a 21 MB arena, `GC_ADDR`), OS time / interrupt / misc services,
threads (the scheduler's task threads on PSP threads, the retrace callback on a vblank thread),
the matrix library (the SDK's C functions), ARAM as RAM with an immediate DMA queue, the pad, and
the disc as the `data/` tree. With two zero-filled placeholder files the game reaches its main
loop, runs the sound driver and message init, and then wanders in its own code over the fake
data (message width scans over a missing font), which is what garbage input does; real,
converted data is the next requirement.

Fixed on the way: five functions that fall off their end (GCC 2.95 returned the last call's
value; GCC 15 makes that unreachable code and the game spun) now return under `RE4_PORT`.

Still stubs (about 210 entry points): GX (the renderer), AX / MIX / SYN / SEQ (sound), CARD
(saves), the CRI movie player, `OSLink` (REL modules: they compile but are not loadable),
`yz2Decode_Decode` (the archive decoder needs a C port of `src/game/yz2asm.cpp`). Next, in
order: the asset converters (every archive format to little-endian: the reconstructed headers
are the spec), the yz2 decoder, static linking of the modules behind `OSLink`, then GX on sceGu
and the sound driver on sceSas.
