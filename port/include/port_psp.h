#ifndef PORT_PSP_H
#define PORT_PSP_H

// The PSP kernel calls the platform layer uses, declared here with plain C types: the pspsdk
// headers and the game's headers both define u32 / BOOL and cannot be included together.
// Signatures follow the pspsdk (pspthreadman.h, pspiofilemgr.h, pspdisplay.h, pspctrl.h).
#ifdef __cplusplus
extern "C" {
#endif

typedef int SceUID_;
typedef int (*PspThreadEntry)(unsigned int args, void* argp);

SceUID_ sceKernelCreateThread(const char* name, PspThreadEntry entry, int initPriority, int stackSize, unsigned int attr, void* option);
int sceKernelStartThread(SceUID_ thid, unsigned int arglen, void* argp);
int sceKernelResumeThread(SceUID_ thid);
int sceKernelSuspendThread(SceUID_ thid);
int sceKernelSleepThread(void);
int sceKernelWakeupThread(SceUID_ thid);
int sceKernelExitDeleteThread(int status);
int sceKernelTerminateDeleteThread(SceUID_ thid);
int sceKernelDeleteThread(SceUID_ thid);
int sceKernelGetThreadId(void);
int sceKernelDelayThread(unsigned int usec);
SceUID_ sceKernelCreateSema(const char* name, unsigned int attr, int initVal, int maxVal, void* option);
int sceKernelWaitSema(SceUID_ semaid, int signal, unsigned int* timeout);
int sceKernelSignalSema(SceUID_ semaid, int signal);
long long sceKernelGetSystemTimeWide(void);
int sceKernelExitGame(void);

int sceAudioChReserve(int channel, int samplecount, int format);
int sceAudioChRelease(int channel);
int sceAudioOutputPannedBlocking(int channel, int leftvol, int rightvol, void* buffer);

int sceDisplayWaitVblankStart(void);
unsigned int sceDisplayGetVcount(void);

typedef struct PspCtrlData {
    unsigned int timeStamp;
    unsigned int buttons;
    unsigned char lx;
    unsigned char ly;
    unsigned char rsrv[6];
} PspCtrlData;
int sceCtrlSetSamplingCycle(int cycle);
int sceCtrlSetSamplingMode(int mode);
int sceCtrlPeekBufferPositive(PspCtrlData* data, int count);
#define PSP_CTRL_SELECT 0x000001
#define PSP_CTRL_START 0x000008
#define PSP_CTRL_UP 0x000010
#define PSP_CTRL_RIGHT 0x000020
#define PSP_CTRL_DOWN 0x000040
#define PSP_CTRL_LEFT 0x000080
#define PSP_CTRL_LTRIGGER 0x000100
#define PSP_CTRL_RTRIGGER 0x000200
#define PSP_CTRL_TRIANGLE 0x001000
#define PSP_CTRL_CIRCLE 0x002000
#define PSP_CTRL_CROSS 0x004000
#define PSP_CTRL_SQUARE 0x008000

SceUID_ sceIoOpen(const char* file, int flags, int mode);
int sceIoClose(SceUID_ fd);
int sceIoRead(SceUID_ fd, void* data, unsigned int size);
long long sceIoLseek(SceUID_ fd, long long offset, int whence);
typedef struct PspIoStat {  // SceIoStat
    int st_mode;
    unsigned int st_attr;
    long long st_size;
    unsigned char st_ctime[16];
    unsigned char st_atime[16];
    unsigned char st_mtime[16];
    unsigned int st_private[6];
} PspIoStat;
int sceIoGetstat(const char* file, PspIoStat* stat);
SceUID_ sceIoDopen(const char* dirname);
int sceIoDclose(SceUID_ fd);
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR 0x0003
#define PSP_O_CREAT 0x0200
#define PSP_O_TRUNC 0x0400
int sceIoWrite(SceUID_ fd, const void* data, unsigned int size);
int sceIoRemove(const char* file);
int sceIoMkdir(const char* dir, int mode);
#define PSP_SEEK_SET 0

#define PSP_THREAD_ATTR_VFPU 0x00004000
#define PSP_THREAD_ATTR_USER 0x80000000

#ifdef __cplusplus
}
#endif

// port/src/port_log.cpp
#ifdef __cplusplus
extern "C" void port_log(const char* fmt, ...);
extern "C" void port_trace(const char* fmt, ...);  // stdout only, never the screen
#else
void port_log(const char* fmt, ...);
void port_trace(const char* fmt, ...);
#endif

#endif
