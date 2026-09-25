#ifndef PORT_H
#define PORT_H

// Constructs that only the matching build must see (docs/matching.md: register pins, section
// anchors, the locked-cache address). The matching build expands them to the original tokens; the
// port build (RE4_PORT, tools/port, port/) expands them to nothing or to its own equivalent. The
// matching forms emit no instruction.
#ifndef RE4_PORT
#define REG_PIN(r) asm(r)      // `register T x REG_PIN("r30");`: the allocator's colour choice
#define ASM_ANCHOR(s) asm(s)   // `ASM_ANCHOR(".section .data; .balign 8");`: data placement / .comm
#define UNIT_INLINE inline     // an `inline` member defined in a unit that other units call (GCC 2.95 emitted it)
#define DOL_STATIC static      // a file-local DOL function the REL modules import by symbol name (the port exports it)
#define ASM_TIE_F(mem, val) asm("" : "=m"(mem) : "f"(val))  // ties a float to a memory slot (em2d)
#define ASM_USE_F(val) asm volatile("" : : "f"(val))          // keeps a float register live (em39)
#define LC_BASE 0xE0000000     // the Gekko locked cache (trans: skinning matrix palette, pendulum: scratch)
// The fixed main-memory map (main_mem.cpp SystemMemInit, read.cpp, dvd.cpp, ...) and the "inside
// MEM1" pointer tests. The matching forms are the original expressions (a parenthesised literal
// or the same tokens with the argument substituted); the port maps the addresses into its arena.
#define GC_ADDR(a) (a)
#define GC_PTR_BAD(p) (u32) p < 0x80000000 || (u32) p > 0x82FFFFFF
#define GC_PTR_BAD_S(p) (s32) p >= 0 || (u32) p > 0x82FFFFFF
#define GC_PTR_GOOD(p) (u32) p >= 0x80000000 && (u32) p <= 0x82FFFFFF
#define GC_PTR_HIGH(p) (u32) p > 0x82FFFFFF
#define GC_ADDR_BAD(a) a < 0x80000000 || a > 0x82FFFFFF
#define GC_PTR_OUT(p) (u32) p - 0x80000000 > 0x02FFFFFF
#define GC_PTR_LOW(p) (u32) p < 0x80000000
#define GC_LOWMEM 0x80000000  // the OS low-memory block (bus clock at +0xF8)
// The PS1-style ordering tables (libgpu.cpp ClearOTagR / AddPrim): a link is a work pointer (bit 31
// set, MEM1) or the address of a table slot with bit 31 cleared.
#define OT_SLOT(p) (u32) p & 0x7FFFFFFF
#define OT_PTR(v) v | 0x80000000
#define OT_IS_WORK(v) (s32) v < 0
#define OT_IS_SLOT(v) (s32) v >= 0
// A load-time fix-up of file data the port converts in place (areas: runtime and file structs share
// the layout): the matching build binds the pointer as it is.
#define PORT_FIX(fn, p) p
// An RGBA word (0xRRGGBBAA as a number) stored into a u8[4] colour array: the bytes land in
// memory order on the GameCube; the port swaps so color[0] stays R.
#define RGBA_BYTES(v) v
#else
#define REG_PIN(r)
#define ASM_ANCHOR(s)
#define UNIT_INLINE
#define DOL_STATIC
#define ASM_TIE_F(mem, val) ((void) 0)
#define ASM_USE_F(val) ((void) 0)
// The ProDG build read these through its own libc headers wherever a unit needed them.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
extern unsigned char port_locked_cache[0x4000];  // port/src/port_mem.cpp: the 16 KB the locked cache had
#define LC_BASE ((unsigned long) port_locked_cache)
// port/src/port_gcmem.cpp: the GameCube's RAM above the ELF (0x80350000..0x81800000) is one
// array, the dev-kit region above it (USB / debug buffers) a small second one. GC_ADDR is an
// address constant, usable in static initialisers.
#define GC_ARENA_START 0x80350000ul
#define GC_ARENA_END 0x81800000ul
#define GC_DEV_START 0x81800000ul
#define GC_DEV_SIZE 0x40000ul
extern unsigned char port_gcmem[];
extern unsigned char port_devmem[];
#define GC_ADDR(a) ((unsigned long) (a) >= GC_DEV_START ? (void*) (port_devmem + ((unsigned long) (a) - GC_DEV_START)) \
                                                        : (void*) (port_gcmem + ((unsigned long) (a) - GC_ARENA_START)))
// Any PSP user-memory address (0x08800000.. the 56 MB of a large-memory model).
#define GC_PTR_OK(p) (((unsigned long) (p) - 0x08800000ul) < 0x03800000ul)
#define GC_PTR_BAD(p) !GC_PTR_OK(p)
#define GC_PTR_BAD_S(p) !GC_PTR_OK(p)
#define GC_PTR_GOOD(p) GC_PTR_OK(p)
#define GC_PTR_HIGH(p) !GC_PTR_OK(p)
#define GC_ADDR_BAD(a) !GC_PTR_OK(a)
#define GC_PTR_OUT(p) !GC_PTR_OK(p)
#define GC_PTR_LOW(p) !GC_PTR_OK(p)
extern unsigned char port_lowmem[0x100];  // port_gcmem.cpp: the bus clock word at +0xF8
#define GC_LOWMEM ((unsigned long) port_lowmem)
// Ordering tables: PSP addresses are positive, so the slot links carry bit 31 instead.
#define OT_SLOT(p) ((u32) (p) | 0x80000000u)
#define OT_PTR(v) ((u32) (v) & 0x7FFFFFFFu)
#define OT_IS_WORK(v) !((u32) (v) & 0x80000000u)
#define OT_IS_SLOT(v) (((u32) (v) & 0x80000000u) != 0)
#define PORT_FIX(fn, p) fn(p)
#define RGBA_BYTES(v) __builtin_bswap32((unsigned int) (v))
#ifdef __cplusplus
struct LightAreaHed;
struct BlockHeader;
extern "C" LightAreaHed* port_fix_light_area(LightAreaHed* p);  // port/src/port_fix.cpp
extern "C" BlockHeader* port_fix_block(BlockHeader* h);
extern "C" void* port_fix_sce_at(void* p);  // an AEV / ITA file
class cSatFile;
extern "C" cSatFile* port_fix_sat(cSatFile* f);  // a room collision (SAT) file
extern "C" void port_fix_sat_native(cSatFile* f);  // a SAT built at run time: registered as already native
extern "C" struct FlrAtHead* port_fix_flr(struct FlrAtHead* p);  // a room floor attribute (FSE) file
#endif
#endif

#endif
