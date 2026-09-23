// Host test build of the port's units: the game's 32-bit integer types on a 64-bit host
// (include/types.h says `unsigned long`, which is 64 bits here).
#ifndef TYPES_H
#define TYPES_H
#include "port.h"
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef signed long long s64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;
typedef int BOOL;
#ifndef NULL
#define NULL 0
#endif
#include "port_be.h"
#endif
