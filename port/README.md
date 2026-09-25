# RE4 PSP port

The second build target of this repository: the game sources of the matching tree compiled with
the PSP toolchain against a platform layer written for the PSP, instead of the Nintendo SDK, CRI
and SN runtime that `src/lib` reproduces. The matching build (`configure.py` + `ninja`, byte-
identical to the GameCube discs) is not touched: every accommodation the port needs in `src/` and
`include/` is behind `RE4_PORT`, expands to the original tokens when it is not defined, and
`tools/port/linecheck.py` proves no `__LINE__` value moved (the assert strings are part of the
bytes).

## Setup (Linux)

```sh
# toolchain: the pspdev release (psp-gcc 15, newlib, pspsdk); cmake / ninja from pip if the distro lacks them
mkdir -p ~/pspdev && curl -L https://github.com/pspdev/pspdev/releases/latest/download/pspdev-ubuntu-latest-x86_64.tar.gz | tar xz -C ~/pspdev
python3 -m venv ~/pspdev/venv && ~/pspdev/venv/bin/pip install cmake ninja websockets pillow
export PSPDEV=$HOME/pspdev/pspdev PATH=$PSPDEV/bin:$HOME/pspdev/venv/bin:$PATH
# emulator: the distro's ppsspp package (PPSSPPSDL and PPSSPPHeadless)
```

Testing on Linux goes through `tools/port/ppsspp.py`, which boots the EBOOT in PPSSPPHeadless
(memory stick folder `~/.ppsspp`) and drives it through the emulator's WebSocket debugger:

```sh
python tools/port/ppsspp.py -t 300 wait 30 hold cross 1 wait 20 shot title.png      # boot, press, screenshot
python tools/port/ppsspp.py -j wait 60 where peek '*pG' 0x40                        # JIT; thread PCs; memory
python tools/port/ppsspp.py wait 5 break _Z9titleExitP9TitleWork waitbreak 60 regs  # breakpoints, registers
FULLLOG=1 ... # the emulator's full HLE log in build/port/headless.log (needed for `shot`, which reads
              # the display buffer out of VRAM: the software renderer is used for that)
```

`tools/port/sym.py ADDR...` maps PCs to functions through `build/port/re4.nm` (`psp-nm -n
build/port/re4`). Writing a path into `dumpPath` (`poke _ZL8dumpPath ms0:/PSP/GAME/RE4/f.raw`) makes
the GX layer save the next frame and trace every draw of it (`[draw]` lines: vertices, matrices, TEV
stages, texture) into the log. `port/run-headless.sh` is the plain boot-and-print variant.

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

## State (2026-09-23, evening)

All 661 game units compile; the 293 DOL units and the 105 game modules (the 9 debug tool
modules are left out of the build) link into one EBOOT that boots from the memory-stick layout.
The platform layer: memory map and heaps (the SDK's OSAlloc over a 21 MB arena, `GC_ADDR`),
OS time / interrupt / misc services, threads (the scheduler's task threads on PSP threads, the
retrace callback on a vblank thread), the matrix library (the SDK's C functions), ARAM as RAM
with an immediate DMA queue, the pad, the `data/` tree as the disc, the yz2 decoder in C
(`port_yz2.cpp`, verified against `tools/port/yz2.py`), the modules statically linked behind
`OSLink` (`tools/port/modlink.py`), and the renderer `port_gx.cpp`: GX on the GE with the vertex
assembly on the CPU (immediate mode and disc display lists, big-endian mesh data read as-is),
GameCube texture formats converted on first use (CMPR to DXT1), stage-0 TEV, blend / depth /
cull / alpha / fog state.

Big-endian file data is read in place through `include/port_be.h`'s field types on the
file-resident structures. Marked so far, following the boot and title-screen path: the core
archive table, texture palettes (and their five relocation sites), the texture registry, the
player / title / option / tagged archives, the message tables and message code streams, the
id-system records (with `BeVec` for file vectors and `BEVEC_PTR` where a `Vec*` is wanted),
Hermite curves and spline paths, the effect sequence / record / animation structures, the light
files, the camera data, the model headers with their relocation, the shape channels, the
motion headers, sequences and the ten key layouts, the sound tables and room sound headers.
Room data (2026-09-23, night): the RTP way points, the EMI placement records (with the
`BEVEC_PTR` sites in the enemy modules), the ESL enemy list read into the save block, the ITM
item model pack, the ETM / ETS etc models and list, the room archive table, the rumble tables,
the font file header, the event file headers, the sub-screen archives, the op message sequence
table and the map room sub-files. Where a file structure is also a runtime structure (the
trigger areas of SAR / BLK / AEV / ITA, whose `AreaData` bodies are floats) the loader binds it
through `PORT_FIX(fn, p)` (port.h) and `port/src/port_fix.cpp` byte-swaps the file's fields in
place once, by record type for the scenario areas. The PS1-style ordering tables (libgpu.cpp:
work links with bit 31 set, table slots with it cleared) go through `OT_SLOT / OT_PTR /
OT_IS_WORK / OT_IS_SLOT`; the port marks the slots with bit 31 instead, since PSP addresses are
positive. `FILE_OFS / FILE_PTR` convert pointers back to file offsets and offsets held in pointer
fields; `VARG` passes a be_ field through a variable argument list. Still unmarked: the event
packet stream (EvtPacket and its payloads), the sound driver's wave tables, the save records
(CARD is a stub anyway), the debug tools' own views of the same files (not built).

The renderer is checked on the host: `python3 tools/port/gxtest.py` builds `build/port/gx_host`
(the GX layer with `port/test/gx_fake_gu.cpp` recording what reaches the GE in place of the
GE, driven by `port/test/gx_host.cpp`'s command script) and compares it with its own references:
every GameCube texture format tiled independently (I4 .. RGBA8, the three palette formats under
C4 / C8 / C14X2, CMPR re-blocked as DXT1 and decoded on both sides), disc display lists with
indexed big-endian attributes, per-vertex matrix indices and the five packed colour formats,
lists that set their own CP registers (VCD / VAT / strides) with XF / BP loads in between,
quads, strips, fans, lines and points, immediate mode with s16 / s8 / colour bytes, texture
matrices, recorded display lists, the render-state mappings and the projection z fix.

With two zero-filled placeholder files the game runs its boot and dies in the font setup on the
font it could not load, which is the placeholder data's limit. `CPUCore = 0` (the interpreter)
in PPSSPP's ini makes the emulator report guest faults with the PC (`build/port/re4.nm` maps it
to a function); the JIT reads zeros silently.

The memory card (`port/src/port_card.cpp`) is a 16 Mbit card in slot A whose files live under
`card/` next to the data tree (`ms0:/PSP/GAME/RE4/card/`, created at the first probe): the
save's data in "<name>", its CardStat in "<name>.stat"; every asynchronous call completes at
once. The saves are this build's own structures, not GameCube saves. `tools/port/cardtest.py`
checks the create / write / read / status / delete round trip on the host. With the placeholder data the
boot now runs on past the font (the JIT reads the missing font as zeros) into the missing core
archive, whose garbage relocation breaks the game heap (the debug display's OSCheckHeap
messages): the placeholder data's limit, not a port fault.

The CRI movie player (`port/src/port_movie.cpp`) is a handle that reports "play end" on its
first frame: the title screen's attract movies and the openings pass in a frame (the generated
NULL-handle stub crashed in cSofdec's interface-table calls). Decoding the .sfd streams is later
work.

Lighting is computed on the CPU in `port_gx.cpp` (the game's lights are in view space and its
vertices carry matrix indices, which the GE's lights cannot follow): the channel controls,
material / ambient registers, the eight light objects with the SDK's spot and distance
attenuation tables, normals through the normal matrices (GXLoadNrmMtxImm), GX_DF_CLAMP /
GX_AF_SPOT. Framebuffer copies (GXCopyTex) read the frame back from the GE into an 8888 buffer
of the copy's size, keyed by the game's destination address, which the texture cache hands out
when that address is bound as a texture; A8 copies keep the alpha, Z copies the depth. Both are
covered by `tools/port/gxtest.py`.

Sound: `port/src/port_ax.cpp` is the AX voice mixer and the MIX volume library the game's own
sound driver (game/snd_*.cpp) programs, on a PSP audio thread. Every 5 ms AX frame it runs the
driver's registered callback (voice manager, stream player, sequencer), then decodes the running
voices (GameCube DSP-ADPCM from the ARAM buffer, or PCM), resamples them by their source ratio,
applies the MIX volumes and pan, sums them and resamples the 32 kHz frames to the 44.1 kHz
output. The driver reads a voice's state and current / end addresses straight from the voice
block (as one u32 at the Hi half), which the mixer keeps up to date. The driver's file
structures (the SIT / RIT / stream headers, the wavetable header and the SYN WT records the
sound banks carry) are marked big-endian. OSDisableInterrupts / OSRestoreInterrupts became a
lock (port_os.cpp): the driver shares its state between the game's threads and the audio
frame, which the GameCube ran as an interrupt; the audio frame runs under the lock with the
driver's own Enable / Restore calls bypassed. `tools/port/axtest.py` checks the mixer on the
host (ADPCM encoded in Python: nibble order, headers, predictor, one-shot end, loop state,
the two resampler modes, pan / volume). Not done: the MIDI synthesiser (SYN / SEQ: the
sequenced BGM stays silent), the aux reverb / chorus sends, the low-pass filter, the volume
envelope.

Not yet: multi-stage TEV / indirect textures, the MIDI synthesiser, movie decoding.

## State (2026-09-24): retail data, the title screen and the first room

Run on Linux against the retail discs (`Resident Evil 4 (USA)`, disc 1 + 2, G4BE08 like the
debug build; their data formats have matched the debug code so far). With the retail data the
game boots, passes the memory-card check (an empty card: the "create system file" prompt is
answered with cross), plays the warning / Dolby / logo screens, renders the title screen (the
artwork, the "4" logo, START / LOAD / OPTIONS, the copyright line), the load-game typewriter
screen, the debug room-select menu of this build, and enters the first room (r120): the stage
overlay `rel/st1_0.rel` links, the player archive and `st1/r120.das` load, and the room's own
loaders start to run. Fixed on the way (all behind RE4_PORT or in the port layer):

- File headers the DVD layer reads in front of every multi-part file (`DvdHeader`), the sub-screen
  archive table (`CardArc`), the sound block ISS offset, the REL header (`OSModuleInfo` /
  `OSModuleHeader`: id, sizes) and the enemy archives' embedded REL offset, the room-jump table
  (`CRoomInfo`, `cRoomJmp`), the effect archive tables (`EffData`, `EffIdTbl`, `EffOfsTbl`,
  `EffEfmEnt`, `SstList`, `SstData`) are big-endian now; the room collision (SAT) files are
  byte-swapped in place once at binding (`port_fix_sat`).
- Two layout assumptions of GCC 2.95: `MessageControl`'s slot accessor computed `this + 4`
  (the vptr came last there, first here: `getMes` / `MES` use `m_Msg`), and the halfword-over-
  bytes unions `room_id` / `stage_no` / `room_no` (`GlobalWork`, `RoomSave`, `RoomNo_next`,
  `room_id_prev`) keep stage in the high byte: the byte order of those unions is swapped for the
  port, `G_ROOM_ID` / `G_ROOM_ID_PREV` read the halfword.
- The GX layer: texture coordinate generation (`GXSetTexCoordGen`: position / normal sources,
  3x4 matrices with the q divide, post matrices, per-vertex texture matrix indices), textures
  padded or halved to the GE's power-of-two sizes up to 512 (`fitTexture`; framebuffer copies
  are resampled straight to that size), TLUTs copied at `GXLoadTlut` (the game builds the
  object on the stack), the texture cache keeps its conversions across `GXInvalidateTexAll` and
  re-checks a content signature instead (its victim search also always preferred slot 0), a
  3 MB vertex ring instead of `sceGuGetMemory` (a room's geometry overflowed the 512 KB list),
  `TEXGet`, texture objects with garbage sizes / addresses are refused (they crashed the
  emulator's software renderer).
- The scheduler on PSP threads: a background (ISR) task's thread blocks in disc reads, so the
  main thread waits while such a task is `TASK_RUN` (`TaskSchedulerMain`), `TaskKill` cancels a
  task killed from another thread instead of exiting the caller, the DVD read holds no lock
  across the read (a killed reader left it taken), the hang detector (`haltExecCheck`) is off.
- Retail has no `etc/moji8.tpl` (the debug text font) and no `debug/roomInfo.dat` (the
  room-jump table): `gamedata.py synthesize` writes both (an 8x16 console font; one jump point
  per room found under `stN/`).

Later the same day the first room came up: r120 loads completely (its model info manager, room
headers, effect data, SAT block tree, blend tables, shadow and scroll headers converted), the
game loop runs, and the opening cutscene (the police car, Leon in the back seat) renders in 3D
with textures. Fixed on the way:

- The sofdec movie player parks 5 MB of ARAM beyond the port's 8 MB ARAM buffer: skipped, the
  movie player is a stub anyway.
- The display list and the vertex ring are finished mid-frame when nearly full (`listRoomCheck`,
  `pg_get_memory`): a room frame's draws did not fit the 512 KB list, and the overrun corrupted
  libpspgu's list state (the GE interrupt handler then jumped to a vertex-format word, seen in
  the emulator's log as `CPU Jump: Invalid exec address 1200019f`).
- The camera cut's Hermite export (`CameraControl::HermiteExport`) writes the camera motion in
  the motion file's byte order (big-endian) since the motion key reader expects file data, and
  `CameraMotion` reads its header through the be_ types: the cutscene camera was NaN before,
  which dropped every draw of the room.
- `ppsspp.py` deletes the port's log before each run (the emulator ignores `PSP_O_TRUNC`) and
  the `[ge]` stats line is followed by a per-phase time line (vertex transform, draw state,
  draw issue, GE wait).

Where the time goes (emulated PSP time, i.e. what the real machine would see): the cutscene
frame is ~330 ms, of which ~320 ms is the CPU vertex assembly (`drawVertices`: the generic
attribute parser, per-vertex lighting and texgen, ~3000 instructions per vertex at 33k vertices
per frame). The path to a playable frame rate is hardware T&L: `GU_TRANSFORM_3D` with the GE's
model / view / projection matrices and lights, and the models' vertex arrays converted once at
load to a native GE vertex format (the disc's display lists reference indexed arrays, which
the GE can consume directly as 16-bit indexed vertices).

Open: skinned models show a few black triangles (Leon's face), "::destroy() ERROR, INVALID"
on the debug console, START during the cutscene (the event skip fast-forward) ends the
emulator; the synthesized debug font draws with the wrong glyphs (the glyph index mapping);
the debug menu of this build has to be dismissed with cross; thin strips at the screen edges on
2D screens; the movie player and the sound synthesis are stubs.

