// port/src/port_hw: the GameCube hardware the game touches directly, as plain memory or no-ops:
// data cache maintenance, the locked cache, performance counters, the audio interface and the
// auxiliary RAM (ARAM) with its DMA queue.
#include "types.h"
#include "port.h"
#include "port_psp.h"
#include <dolphin/os.h>
#include <dolphin/ar.h>
#include <string.h>

// ARAM: 16 MB on the GameCube; the sound driver alone takes 7 MB (snd.cpp ARAlloc(0x6FC000)), the
// room block streamer parks blocks in the rest. 8 MB here until the memory budget is settled.
#define PORT_ARAM_SIZE 0x800000
static unsigned char port_aram[PORT_ARAM_SIZE] __attribute__((aligned(64)));
static u32 aramTop = 0x4000;  // the SDK reserves the first 16 KB

extern "C" {

// ---- caches: PSP memory the GE reads must be written back with sceKernelDcacheWritebackRange;
// the GX layer will do that where it matters. Nothing to do for the CPU side.
void DCFlushRange(void* addr, u32 nBytes) {}
void DCFlushRangeNoSync(void* addr, u32 nBytes) {}
void DCInvalidateRange(void* addr, u32 nBytes) {}
void DCStoreRange(void* addr, u32 nBytes) {}
void DCStoreRangeNoSync(void* addr, u32 nBytes) {}
void LCEnable(void) {}

// ---- performance counters / special registers (debug statistics only)
u32 PPCMfpmc1(void) { return 0; }
void PPCMtmmcr0(u32 v) {}
void PPCMtmmcr1(u32 v) {}
void PPCMtmsr(u32 v) {}
void PPCMtpmc1(u32 v) {}
void PPCMtpmc2(u32 v) {}
void PPCMtpmc3(u32 v) {}
void PPCMtpmc4(u32 v) {}
void PPCSync(void) {}

// ---- audio interface (the AX driver is stubbed separately)
void AIInit(u8* stack) {}
void AIReset(void) {}

// ---- ARAM
u32 ARInit(u32* stack_index_addr, u32 num_entries)
{
    aramTop = 0x4000;
    return aramTop;
}

u32 ARAlloc(u32 length)
{
    u32 addr = aramTop;
    if (addr + length > PORT_ARAM_SIZE) {
        port_log("[port] ARAlloc(%x): ARAM buffer of %x exhausted\n", (unsigned) length, PORT_ARAM_SIZE);
        return 0;
    }
    aramTop += length;
    return addr;
}

u32 ARGetBaseAddress(void) { return 0x4000; }
u32 ARGetSize(void) { return PORT_ARAM_SIZE; }

void ARQInit(void) {}
void ARQFlushQueue(void) {}

// The DMA queue: copies happen at once and the callback runs before the call returns.
void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length, ARQCallback callback)
{
    request->owner = owner;
    request->type = type;
    request->priority = priority;
    request->source = source;
    request->dest = dest;
    request->length = length;
    request->callback = callback;
    if (type == 0) {  // ARQ_TYPE_MRAM_TO_ARAM
        if (dest + length <= PORT_ARAM_SIZE) {
            memcpy(port_aram + dest, (const void*) source, length);
        }
    } else {          // ARQ_TYPE_ARAM_TO_MRAM
        if (source + length <= PORT_ARAM_SIZE) {
            memcpy((void*) dest, port_aram + source, length);
        }
    }
    if (callback) {
        callback((u32) request);
    }
}

}  // extern "C"
