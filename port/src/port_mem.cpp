// port/src/port_mem: memory that the GameCube had as hardware and the port keeps as plain RAM.
#include "port.h"

// The Gekko's 16 KB locked cache (LCEnable in main.cpp; trans.cpp keeps the skinning matrix
// palette there, pendulum.cpp its collision scratch). LC_BASE (include/port.h) resolves to it.
unsigned char port_locked_cache[0x4000] __attribute__((aligned(64)));
