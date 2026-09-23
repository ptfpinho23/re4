// Host test build: no PSP kernel; the log goes to stderr.
#ifndef PORT_PSP_H
#define PORT_PSP_H
#include <stdio.h>
#include <stdarg.h>
static inline void port_log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
// the kernel calls the sound layer makes at start-up: never reached on the host (no AXInitEx)
typedef int SceUID_;
typedef int (*PspThreadEntry)(unsigned int args, void* argp);
static inline SceUID_ sceKernelCreateThread(const char* n, PspThreadEntry e, int p, int s, unsigned int a, void* o) { return -1; }
static inline int sceKernelStartThread(SceUID_ t, unsigned int l, void* a) { return 0; }
static inline int sceAudioChReserve(int c, int n, int f) { return -1; }
static inline int sceAudioOutputPannedBlocking(int c, int l, int r, void* b) { return 0; }
// file I/O, provided by the test program over POSIX (card_host.cpp)
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR 0x0003
#define PSP_O_CREAT 0x0200
#define PSP_O_TRUNC 0x0400
typedef struct PspIoStat { int st_mode; unsigned int st_attr; long long st_size; unsigned char rest[64]; } PspIoStat;
extern "C" SceUID_ sceIoOpen(const char* file, int flags, int mode);
extern "C" int sceIoClose(SceUID_ fd);
extern "C" int sceIoRead(SceUID_ fd, void* data, unsigned int size);
extern "C" int sceIoWrite(SceUID_ fd, const void* data, unsigned int size);
extern "C" long long sceIoLseek(SceUID_ fd, long long offset, int whence);
extern "C" int sceIoRemove(const char* file);
extern "C" int sceIoMkdir(const char* dir, int mode);
extern "C" int sceIoGetstat(const char* file, PspIoStat* stat);
extern "C" SceUID_ sceIoDopen(const char* dirname);
extern "C" int sceIoDclose(SceUID_ fd);
#endif
