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
#endif

#endif
