// port/src/port_thread: Dolphin OS threads on PSP kernel threads.
//
// The game's scheduler (scheduler.cpp) runs one task thread at a time: the main thread resumes a
// task, the task's hook suspends the main thread, and when the task sleeps or exits it resumes
// the main thread first. Task threads run at a priority one step above the main thread so a
// resume hands control over at once, as on the GameCube. The OSThread structure is the game's
// (embedded in TASK); the port keeps the PSP thread id and the entry point in its context area
// and a small registry to find the OSThread of the calling PSP thread.
#include "types.h"
#include "port.h"
#include "port_psp.h"
#include <dolphin/os.h>
#include <string.h>

#define PSP_PRIO_BASE 16  // GameCube priority 16 (the main thread) == PSP priority 32
#define REGISTRY_MAX 16

struct PortThread {  // overlays OSThread::context (0x2C8 bytes)
    int uid;
    int started;
    void* (*func)(void*);
    void* param;
    void* result;
};

static OSThread port_main_thread;  // the GameCube main thread, i.e. the PSP thread that runs main()
static OSThread* registry[REGISTRY_MAX];
#define PT(t) ((PortThread*) &(t)->context)

static void registryAdd(OSThread* t)
{
    for (int i = 0; i < REGISTRY_MAX; i++) {
        if (registry[i] == NULL || registry[i] == t) {
            registry[i] = t;
            return;
        }
    }
    port_log("[port] thread registry full\n");
}

static void registryRemove(OSThread* t)
{
    for (int i = 0; i < REGISTRY_MAX; i++) {
        if (registry[i] == t) {
            registry[i] = NULL;
        }
    }
}

static OSThread* registryFind(int uid)
{
    for (int i = 0; i < REGISTRY_MAX; i++) {
        if (registry[i] && PT(registry[i])->uid == uid) {
            return registry[i];
        }
    }
    return NULL;
}

extern "C" {

static int port_thread_entry(unsigned int args, void* argp)
{
    OSThread* t = *(OSThread**) argp;
    t->state = OS_THREAD_STATE_RUNNING;
    PT(t)->result = PT(t)->func(PT(t)->param);
    t->state = OS_THREAD_STATE_MORIBUND;
    registryRemove(t);
    return 0;
}

BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize, OSPriority priority, u16 attr)
{
    memset(thread, 0, sizeof(*thread));
    PT(thread)->func = func;
    PT(thread)->param = param;
    thread->priority = priority;
    thread->base = priority;
    thread->attr = attr;
    thread->stackBase = (u8*) stack;
    thread->stackEnd = (u32*) ((u8*) stack - stackSize);
    *thread->stackEnd = OS_THREAD_STACK_MAGIC;  // scheduler.cpp StackOverflowCheck reads it
    int pspStack = stackSize < 0x4000 ? 0x4000 : (int) stackSize;
    PT(thread)->uid = sceKernelCreateThread("re4task", port_thread_entry, PSP_PRIO_BASE + priority, pspStack,
                                            PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (PT(thread)->uid < 0) {
        port_log("[port] OSCreateThread: sceKernelCreateThread failed %08x\n", PT(thread)->uid);
        return FALSE;
    }
    thread->state = OS_THREAD_STATE_READY;
    thread->suspend = 1;
    registryAdd(thread);
    return TRUE;
}

s32 OSResumeThread(OSThread* thread)
{
    s32 prev = thread->suspend;
    thread->suspend = 0;
    thread->state = OS_THREAD_STATE_RUNNING;
    if (thread == &port_main_thread || PT(thread)->started) {
        sceKernelResumeThread(PT(thread)->uid);
    } else {
        PT(thread)->started = 1;
        OSThread* arg = thread;
        int r = sceKernelStartThread(PT(thread)->uid, sizeof(arg), &arg);
        if (r < 0) {
            port_log("[port] OSResumeThread: sceKernelStartThread failed %08x\n", r);
            thread->state = OS_THREAD_STATE_MORIBUND;
        }
    }
    return prev;
}

s32 OSSuspendThread(OSThread* thread)
{
    if (PT(thread)->uid == sceKernelGetThreadId()) {
        // A thread suspending itself: the PSP refuses it (and the emulator stops on it). Name the
        // caller; the scheduler's cooperative handshake expects the parent thread here.
        static int n;
        if (++n <= 10) port_log("[port] OSSuspendThread(self) from %p\n", __builtin_return_address(0));
        return 0;
    }
    s32 prev = thread->suspend;
    thread->suspend = prev + 1;
    sceKernelSuspendThread(PT(thread)->uid);
    return prev;
}

void OSInitThreadQueue(OSThreadQueue* queue)
{
    queue->head = NULL;
    queue->tail = NULL;
}

OSThread* OSGetCurrentThread(void)
{
    int me = sceKernelGetThreadId();
    if (PT(&port_main_thread)->uid == 0) {
        PT(&port_main_thread)->uid = me;
        PT(&port_main_thread)->started = 1;
        port_main_thread.state = OS_THREAD_STATE_RUNNING;
        port_main_thread.priority = 16;
        port_main_thread.base = 16;
        registryAdd(&port_main_thread);
    }
    OSThread* t = registryFind(me);
    return t ? t : &port_main_thread;
}

void OSSleepThread(OSThreadQueue* queue)
{
    OSThread* t = OSGetCurrentThread();
    queue->head = t;
    queue->tail = t;
    t->queue = queue;
    t->state = OS_THREAD_STATE_WAITING;
    sceKernelSleepThread();
    t->state = OS_THREAD_STATE_RUNNING;
    t->queue = NULL;
}

void OSWakeupThread(OSThreadQueue* queue)
{
    OSThread* t = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    if (t) {
        sceKernelWakeupThread(PT(t)->uid);
    }
}

// Called by the exiting thread itself (TaskExit / TaskChain pass their OSThread as `val`).
void OSExitThread(void* val)
{
    OSThread* t = OSGetCurrentThread();
    t->state = OS_THREAD_STATE_MORIBUND;
    registryRemove(t);
    sceKernelExitDeleteThread(0);
}

// A yield that lets threads of any priority run (the scheduler spins on it while a background task
// is blocked in a disc read).
void OSYieldThread(void) { sceKernelDelayThread(200); }

void OSCancelThread(OSThread* thread)
{
    thread->state = OS_THREAD_STATE_MORIBUND;
    if (thread->queue) {
        thread->queue->head = NULL;
        thread->queue->tail = NULL;
        thread->queue = NULL;
    }
    registryRemove(thread);
    int r = sceKernelTerminateDeleteThread(PT(thread)->uid);
    port_trace("[port] OSCancelThread uid %08x from thread %d: %08x\n", (unsigned) PT(thread)->uid, sceKernelGetThreadId(), (unsigned) r);
}

// ---- semaphores (the PSP semaphore id lives in `count`)
void OSInitSemaphore(OSSemaphore* sem, s32 count)
{
    sem->count = sceKernelCreateSema("re4sema", 0, count, 0x7FFFFFFF, NULL);
    sem->queue.head = NULL;
    sem->queue.tail = NULL;
}

s32 OSWaitSemaphore(OSSemaphore* sem)
{
    sceKernelWaitSema(sem->count, 1, NULL);
    return 0;
}

s32 OSSignalSemaphore(OSSemaphore* sem)
{
    sceKernelSignalSema(sem->count, 1);
    return 0;
}

}  // extern "C"
