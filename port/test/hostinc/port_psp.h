// Host test build: no PSP kernel; the log goes to stderr.
#ifndef PORT_PSP_H
#define PORT_PSP_H
#include <stdio.h>
#include <stdarg.h>
static inline void port_log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
#endif
