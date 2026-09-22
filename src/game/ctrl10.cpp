// game/ctrl10.cpp: control 0x10 (cCtrl10). No code survives in the shipped build; only the
// ctrl.h range-check string is in its .rodata.

#include "types.h"
#include "cManager.h"
#include "ctrl.h"

// The original .rodata is 8-aligned (0x18 bytes) although only the ctrl.h string survived the link.
ASM_ANCHOR(".section .rodata; .balign 8");
