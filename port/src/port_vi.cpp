// port/src/port_vi: the video interface. The GameCube's retrace interrupt becomes a thread that
// waits for the PSP vblank and calls the game's post-retrace callback (main.cpp
// postVSyncCallback: frame counter, ISR task suspension, the 60-second watchdog).
#include "types.h"
#include "port.h"
#include "port_psp.h"
#include <dolphin/os.h>
#include <dolphin/vi.h>

static VIRetraceCallback postCallback;
static int vblankThread = -1;
static u32 retraceCount;

static int port_vblank_thread(unsigned int args, void* argp)
{
    for (;;) {
        sceDisplayWaitVblankStart();
        retraceCount++;
        if (postCallback) {
            postCallback(retraceCount);
        }
    }
    return 0;
}

extern "C" {

void VIInit(void) {}
void VIConfigure(const GXRenderModeObj* rm) {}
void VISetBlack(BOOL black) {}
void VIFlush(void) {}
void VISetNextFrameBuffer(void* fb) {}
u32 VIGetNextField(void) { return 0; }
u32 VIGetTvFormat(void) { return 0; }  // VI_NTSC
u32 VIGetDTVStatus(void) { return 0; }
u32 VIGetRetraceCount(void) { return retraceCount; }

void VIWaitForRetrace(void)
{
    sceDisplayWaitVblankStart();
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback prev = postCallback;
    postCallback = cb;
    if (cb && vblankThread < 0) {
        // Above the game's task threads (priority 31/32), so the callback preempts them like the interrupt did.
        vblankThread = sceKernelCreateThread("re4vblank", port_vblank_thread, 20, 0x4000,
                                             PSP_THREAD_ATTR_USER, NULL);
        if (vblankThread >= 0) {
            sceKernelStartThread(vblankThread, 0, NULL);
        } else {
            port_log("[port] vblank thread creation failed %08x\n", vblankThread);
        }
    }
    return prev;
}

}  // extern "C"
