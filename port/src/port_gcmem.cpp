// port/src/port_gcmem: the GameCube's main memory as the port keeps it. The game's memory map is
// fixed (main_mem.cpp SystemMemInit: DVD buffers, sound, FIFO, XFB, the core / option / player /
// weapon archives, then the heap up to 0x817F4000); GC_ADDR (include/port.h) maps each of those
// addresses into port_gcmem, and the OS arena the game hands to OSAlloc is the same array.
#include "port.h"

unsigned char port_gcmem[GC_ARENA_END - GC_ARENA_START] __attribute__((aligned(64)));
unsigned char port_devmem[GC_DEV_SIZE] __attribute__((aligned(64)));
