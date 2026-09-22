// port/app/psp_main: the PSP module header for the game executable, the HOME-button exit
// callback and a boot banner. The game's own main() (src/game/main.cpp) is the entry point the
// PSP SDK's crt0 calls. This unit lives outside port/src so the static library never drops it.
#include <pspkernel.h>

PSP_MODULE_INFO("RE4", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
PSP_HEAP_SIZE_KB(-512);

extern "C" void port_log(const char* fmt, ...);

static int exitCallback(int arg1, int arg2, void* common)
{
    sceKernelExitGame();
    return 0;
}

static int callbackThread(SceSize args, void* argp)
{
    int cbid = sceKernelCreateCallback("re4exit", exitCallback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

// Runs before main() (a static constructor): the exit callback thread the PSP needs to honour
// HOME, and a banner that proves the log path works even if main() never gets to print.
__attribute__((constructor)) static void port_boot()
{
    int thid = sceKernelCreateThread("re4cb", callbackThread, 0x11, 0x1000, 0, NULL);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, NULL);
    }
    port_log("[port] RE4 PSP port booting\n");
}
