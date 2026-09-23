// port/src/port_runtime: what SN's runtime and Capcom's asm units gave the GameCube build.
#include "types.h"
#include "port.h"
#include "port_stub.h"
#include <string.h>
#include <new>

// GCC 2.95's names for the global allocation functions: sce_com.cpp calls them directly and the
// tool modules bind class allocators to them (main_mem.cpp's operator new / delete route to the
// game heaps, so these do too).
extern "C" void* __builtin_new(unsigned int size) { return operator new(size); }
extern "C" void* __builtin_vec_new(unsigned int size) { return operator new[](size); }
extern "C" void __builtin_delete(void* p) { operator delete(p); }
extern "C" void __builtin_vec_delete(void* p) { operator delete[](p); }

// game/memset_2.cpp (PowerPC asm): cache-line clears and fills.
extern "C" void memclr_asm(void* dst, u32 n) { memset(dst, 0, n); }
extern "C" void memset_asm(void* dst, int c, u32 n) { memset(dst, c, n); }

// CRI ADX / Sofdec (src/lib): the movie player's file system hookup and output mode. Movies are
// re-encoded for the PSP's own decoder in the port, so these stay no-ops.
extern "C" void ADXGC_SetupDvdFs(void* p) { PORT_STUB("ADXGC_SetupDvdFs"); }
extern "C" void ADXT_SetOutputMono(int sw) { PORT_STUB("ADXT_SetOutputMono"); }

// The SDK's TEX library entry the game uses (texPalette.c TEXGet): descriptor `id` of a TPL the
// caller has already relocated (cTexSys::CalcTplAddr and the per-unit calcTplAddr copies).
#include "tpl.h"
extern "C" TEXDescriptor* TEXGet(TEXPalette* pal, u32 id)
{
    return &pal->descriptorArray[id];
}
