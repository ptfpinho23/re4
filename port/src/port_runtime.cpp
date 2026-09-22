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

// game/yz2asm.cpp (PowerPC asm): Capcom's yz2 archive decoder. yz2code.cpp drives it; the port
// needs a C reimplementation of the 504-line asm (dictionary-based LZ over two frequency models).
extern "C" void yz2Decode_Decode(void* ctx, void* dst, u32 size, void* ev)
{
    PORT_STUB("yz2Decode_Decode");
}

// CRI ADX / Sofdec (src/lib): the movie player's file system hookup and output mode. Movies are
// re-encoded for the PSP's own decoder in the port, so these stay no-ops.
extern "C" void ADXGC_SetupDvdFs(void* p) { PORT_STUB("ADXGC_SetupDvdFs"); }
extern "C" void ADXT_SetOutputMono(int sw) { PORT_STUB("ADXT_SetOutputMono"); }
