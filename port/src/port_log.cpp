// port/src/port_log: the port's own logging (stub reports, OSReport). Every line goes to the PSP's
// stdout descriptor, unbuffered (PPSSPP shows it in its log, psplink forwards it to the host), and
// to an on-screen text console (pspsdk's debug screen), which is all a PSP without a host shows.
// The game's own OSReport traffic is capped on screen so the port's messages stay readable.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pspiofilemgr.h>
#include <pspstdio.h>
#include <pspdebug.h>
#include "port_stub.h"

static int screenReady;
static int gameLinesShown;
#define GAME_LINES_ON_SCREEN 120

extern "C" int port_gx_active(void);
extern "C" void port_gx_overlay_line(const char* text);

static void emit(const char* buf, int n, int fromGame)
{
    sceIoWrite(sceKernelStdout(), buf, n);
    if (port_gx_active()) {
        if (!fromGame) {
            port_gx_overlay_line(buf);
        }
        return;
    }
    if (!screenReady) {
        pspDebugScreenInit();
        screenReady = 1;
    }
    if (fromGame) {
        if (gameLinesShown == GAME_LINES_ON_SCREEN) {
            pspDebugScreenPuts("[port] (further game output only on stdout)\n");
        }
        if (gameLinesShown++ >= GAME_LINES_ON_SCREEN) {
            return;
        }
    }
    pspDebugScreenPuts(buf);
}

static int format(char* buf, int size, const char* fmt, va_list ap)
{
    int n = vsnprintf(buf, size, fmt, ap);
    if (n > size - 1) {
        n = size - 1;
    }
    return n;
}

extern "C" void port_log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = format(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        emit(buf, n, 0);
    }
}

extern "C" void port_stub_report(const char* name, int* once)
{
    *once = 1;
    port_log("[port] unimplemented: %s\n", name);
}

// The SDK's printf. Every unit's HALT / assert path ends here, so it is a strong definition.
extern "C" void OSReport(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = format(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        emit(buf, n, 1);
    }
}
