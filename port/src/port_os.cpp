// port/src/port_os: the Dolphin OS services the game calls, on the PSP. Heaps are the SDK's own
// OSAlloc.c (compiled into this library); threads are in port_thread.cpp.
#include "types.h"
#include "port.h"
#include "port_psp.h"
#include <dolphin/os.h>
#include <string.h>

extern "C" {

// ---- arena: the game's heap range inside port_gcmem
static void* arenaLo = (void*) GC_ADDR(GC_ARENA_START);
static void* arenaHi = (void*) GC_ADDR(0x817F4000);

void* OSGetArenaLo(void) { return arenaLo; }
void* OSGetArenaHi(void) { return arenaHi; }
void OSSetArenaLo(void* p) { arenaLo = p; }
void OSSetArenaHi(void* p) { arenaHi = p; }

u32 OSGetConsoleSimulatedMemSize(void) { return 0x01800000; }  // 24 MB, the retail size
u32 OSGetConsoleType(void) { return OS_CONSOLE_RETAIL; }

void OSInit(void)
{
    port_log("[port] OSInit: arena %p..%p (%u KB), ARAM buffer in port_hw\n", arenaLo, arenaHi,
             (unsigned) (((u8*) arenaHi - (u8*) arenaLo) >> 10));
}

// ---- time: the Gekko time base ran at OS_BUS_CLOCK / 4 = 40.5 MHz; the PSP clock is in µs.
OSTime OSGetTime(void)
{
    long long us = sceKernelGetSystemTimeWide();
    return (OSTime) (us * 81 / 2);
}

OSTick OSGetTick(void) { return (OSTick) OSGetTime(); }

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td)
{
    // Seconds since 2000-01-01 in the SDK's convention; a plain civil-date conversion.
    s64 sec = ticks / (OS_TIMER_CLOCK);
    s64 days = sec / 86400;
    s64 rem = sec % 86400;
    memset(td, 0, sizeof(*td));
    td->hour = (int) (rem / 3600);
    td->min = (int) (rem % 3600 / 60);
    td->sec = (int) (rem % 60);
    td->wday = (int) ((days + 6) % 7);  // 2000-01-01 was a Saturday
    int y = 2000;
    for (;;) {
        int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        int ylen = leap ? 366 : 365;
        if (days < ylen) break;
        days -= ylen;
        y++;
    }
    td->year = y;
    td->yday = (int) days;
    static const int mlen[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    int m = 0;
    while (m < 12) {
        int len = mlen[m] + (m == 1 && leap);
        if (days < len) break;
        days -= len;
        m++;
    }
    td->mon = m;
    td->mday = (int) days + 1;
}

OSTime OSCalendarTimeToTicks(OSCalendarTime* td)
{
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    s64 days = 0;
    int y;
    for (y = 2000; y < td->year; y++) {
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    }
    int leap = (td->year % 4 == 0 && (td->year % 100 != 0 || td->year % 400 == 0));
    days += cum[td->mon] + (td->mon > 1 && leap) + td->mday - 1;
    s64 sec = days * 86400 + td->hour * 3600 + td->min * 60 + td->sec;
    return (OSTime) sec * OS_TIMER_CLOCK;
}

// ---- interrupts: the game masks them around queue updates; the port's callbacks are threads.
BOOL OSDisableInterrupts(void) { return TRUE; }
BOOL OSEnableInterrupts(void) { return TRUE; }
BOOL OSRestoreInterrupts(BOOL level) { return level; }

// ---- errors, alarms, fonts, misc
OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) { return NULL; }

void OSPanic(const char* file, int line, const char* msg, ...)
{
    port_log("[port] OSPanic %s(%d): %s\n", file, line, msg);
    for (;;) {
        sceKernelDelayThread(1000000);
    }
}

void OSInitAlarm(void) {}
void OSSetSaveRegion(void* start, void* end) {}
s32 OSEnableScheduler(void) { return 0; }
BOOL OSGetResetButtonState(void) { return FALSE; }
u32 OSGetProgressiveMode(void) { return 0; }
void OSSetProgressiveMode(u32 on) {}
u32 OSGetSoundMode(void) { return 1; }  // stereo
void OSSetSoundMode(u32 mode) {}

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu)
{
    port_log("[port] OSResetSystem(%d, %u): exiting\n", reset, (unsigned) resetCode);
    sceKernelExitGame();
}

u16 OSGetFontEncode(void) { return 0; }  // ANSI
BOOL OSInitFont(OSFontHeader* fontData) { return FALSE; }
char* OSGetFontTexture(const char* string, void** image, s32* x, s32* y, s32* width) { return (char*) string; }

}  // extern "C"
