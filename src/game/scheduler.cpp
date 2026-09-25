// game/scheduler: the cooperative task scheduler — 18 task slots (TASK), each an OS thread with
// its own stack, run one after another by the main thread every frame (TaskScheduler ->
// TaskSchedulerMain); a task runs until it calls TaskSleep / TaskExit, which hand control back.
// Slot 0 is the game, 1 debug / sub screen / movie tasks, 4 the background (ISR) task, 5..17 the
// scenario tasks (sce_sys). Task flags decide whether a slot keeps running during events and the
// sub screen. (D:/Bio4/Prog/scheduler.cpp)
#ifdef RE4_PORT
#include "port_psp.h"
extern "C" int sceKernelGetThreadId(void);
#endif
#include "types.h"
#include "global.h"
#include "main_mem.h"
#include "db_log.h"
#include "eprintf.h"
#include "os_vi.h"
#include "scheduler.h"
#include <dolphin/gx/GXFifo.h>
#include <dolphin/os.h>
#ifdef RE4_PORT
extern "C" void port_heap_check(const char* stage);  // main.cpp: the game heap consistency, for the port's search of a corruption
#endif

void DbMenuRestoreStopFlag();

TASK Task[TASK_NUM];
static OSSemaphore Sema;
TASK* CTASK_MAIN = (TASK*) -1;
TASK* pCTask = CTASK_MAIN;
OSThread* pParentThread;
static int iTask_exec_flg = 0;

void TaskKill(TASK* t);

// Boot: gives every task slot its stack (one allocation, filled with 0xB3 for the usage check),
// number and thread queue; the scheduler semaphore starts at 0.
void TaskSchedulerInit()
{
    u32 total = 0;
    u8* stack;
    u32 i;

    for (i = 0; i < TASK_NUM; i++) {
        Task[i].StackSize = GetStackSize(i);
        total += Task[i].StackSize;
    }
#line 48 "D:/Bio4/Prog/scheduler.cpp"
    stack = (u8*) MEM_ALLOC(total, 1, 13);
    memset_asm(stack, 0xB3, total);
    for (i = 0; i < TASK_NUM; i++) {
        Task[i].Task_no = i;
        Task[i].Status = 0;
        Task[i].pStack = stack + Task[i].StackSize;
        Task[i].suspend_cnt = 0;
        OSInitThreadQueue(&Task[i].Queue);
        stack = Task[i].pStack;
    }
    OSInitSemaphore(&Sema, 0);
    iTask_exec_flg = 0;
}

// Kills every task (game reset).
void TaskAllClear()
{
    u32 i;

    for (i = 0; i < TASK_NUM; i++) {
        TaskKill(i);
    }
    iTask_exec_flg = 0;
}

// Stack bytes for slot `no`: 0x3000 for slot 2 (the main game task), 0x2000 for 0..4, 0x1800 above.
u32 GetStackSize(int level)
{
    if (level == 2) {
        return 0x3000;
    }
    if ((u32) level > 4) {
        return 0x1800;
    }
    return 0x2000;
}

// Once per frame from the main thread: runs every slot except the ISR one in order (slot 2 first
// restores the debug menu's stop flag); debug_mode 6 prints the stack usage.
void TaskScheduler()
{
    u32 i;
    TASK* t;

    pParentThread = OSGetCurrentThread();
    for (i = 0, t = Task; i <= TASK_ISR; i++, t++) {
        if (i == TASK_ISR) {
            continue;
        }
        if (i == 2) {
            DbMenuRestoreStopFlag();
        }
        pCTask = t;
        TaskSchedulerMain(t);
#ifdef RE4_PORT
        {
            static const char* const slot[] = {"task 0", "task 1", "task 2", "task 3", "task 4"};
            port_heap_check(slot[i < 5 ? i : 4]);
        }
#endif
    }
    pCTask = CTASK_MAIN;
    if (pG->debug_mode == 6) {
        stackUsedCheck();
    }
}

// Runs one task for this frame: skipped while an event holds non-event tasks (Status_flg[1]
// 0x10000000 vs flag bit1) or the sub screen holds (Status_flg[0] 0x100000 vs bit2); TASK_EXEC
// creates and starts its thread, TASK_SLEEP counts down and wakes it, TASK_RUN resumes it; the
// main thread then waits (semaphore for priority > 0xF tasks) until the task sleeps / exits.
void TaskSchedulerMain(TASK* pT)
{
    if (StaFlagChk(pG, STA_SUSPEND) && !(pT->flag & 2)) {
        return;
    }
    if (StaFlagChk(pG, STA_DIEDEMO) && !(pT->flag & 4)) {
        return;
    }
#ifdef RE4_PORT
    // iTaskScheduler runs this with interrupts disabled. On the GameCube that state is the
    // caller's own (the task thread runs with its own), on the PSP it is a lock every thread
    // shares: hand it back while the task runs, or a task that masks interrupts (the disc
    // reader) deadlocks against this thread's wait below.
    BOOL intrLevel = pParentThread == NULL ? OSEnableInterrupts() : 1;
#endif
    switch (pT->Status) {
    case TASK_EXEC:
        OSCreateThread(&pT->Thread, pT->hook, (void*) pT->arg, pT->pStack, pT->StackSize, pT->Priority, 1);
        pT->Status = TASK_RUN;
        OSResumeThread(&pT->Thread);
        GXSetCurrentGXThread();
        break;
    case TASK_SLEEP:
        pT->SleepCtr--;
        if (pT->SleepCtr != 0) {
            return;
        }
        pT->Status = TASK_RUN;
        OSWakeupThread(&pT->Queue);
        GXSetCurrentGXThread();
        break;
    case TASK_RUN:
        OSResumeThread(&pT->Thread);
        GXSetCurrentGXThread();
        break;
    default:
#ifdef RE4_PORT
        if (pParentThread == NULL) OSRestoreInterrupts(intrLevel);
#endif
        return;
    }
    if (pCTask->Priority > 0xF) {
        OSWaitSemaphore(&Sema);
        GXSetCurrentGXThread();
    }
#ifdef RE4_PORT
    // The background (ISR) task has no parent to suspend: on the GameCube its priority keeps the
    // main thread off the CPU until it sleeps, exits or the retrace suspends it. On the PSP its
    // thread blocks in disc reads, which would hand the CPU back with the task still "running";
    // wait for the task to reach one of those states instead.
    if (pParentThread == NULL) {
        // A task whose function returns (a scenario task, whose thread then just ends) keeps
        // TASK_RUN: its thread is moribund by then.
        while (pT->Status == TASK_RUN && pT->Thread.state != OS_THREAD_STATE_MORIBUND) {
            OSYieldThread();
        }
        {
            static int nTr;
            if (++nTr <= 40) port_trace("[port] TaskSchedulerMain(ISR) done: status %x thread state %d\n", pT->Status, (int) pT->Thread.state);
        }
        OSRestoreInterrupts(intrLevel);
    }
#endif
    StackOverflowCheck(pT);
}

// Debug: prints how much of each task stack has been touched (bytes no longer 0xB3).
void stackUsedCheck()
{
    u32 i;
    int y = 0x24;

    eprintf2(9, 0x10, 0x22, 0x24, 0, 6, "   SIZE REST");
    for (i = 0; i < TASK_NUM; i++) {
        u32* p = (u32*) (Task[i].pStack - Task[i].StackSize);
        u32 n = 1;
        p++;
        while (n < (u32) (Task[i].StackSize >> 2) && *p == 0xB3B3B3B3) {
            n++;
            p++;
        }
        y += 0x10;
        eprintf(0x10, y, 0, 6, "%02d %4x %4x %08x", i, Task[i].StackSize, n * 4, Task[i].pStack - Task[i].StackSize);
    }
}

// Panics when the guard word at the bottom of the task's stack was overwritten.
void StackOverflowCheck(TASK* pTask)
{
    if (*(u32*) (pTask->pStack - pTask->StackSize) != 0xDEADBABE) {
        OSReport("***************************************\n");
        OSReport("Stack overflow in Thread %d !!\n", pTask->Task_no);
        OSReport("***************************************\n");
        OSPanic("D:/Bio4/Prog/scheduler.cpp", 217, "End of biohazard4");
    }
}

// Thread entry of every task: suspends the scheduler thread, sets the GQR registers for the
// paired-single loads, and calls the task function with its argument.
void* TaskExec_hook(void* value)
{
    if (pParentThread != NULL) {
        OSSuspendThread(pParentThread);
    }
#ifndef RE4_PORT  // GQR2..5 = u8/u16/s8/s16, scale 0: the port's PSQ_* macros assume exactly this
    asm("li 3, 4\n"
        "oris 3, 3, 4\n"
        "mtspr 914, 3\n"
        "li 3, 5\n"
        "oris 3, 3, 5\n"
        "mtspr 915, 3\n"
        "li 3, 6\n"
        "oris 3, 3, 6\n"
        "mtspr 916, 3\n"
        "li 3, 7\n"
        "oris 3, 3, 7\n"
        "mtspr 917, 3"
        :
        :
        : "r3");
#endif
    GXSetCurrentGXThread();
    pCTask->pFunc((int) value);
    return NULL;
}

// Starts `func(arg)` in slot `prio` (fails with an error when the slot is busy): the thread is
// created on the next scheduler pass; flag 6 (skipped by events and the sub screen), priority 0xF.
TASK* TaskExec(int prio, TaskFunc func, int arg)
{
    TASK* t;

    if (TaskStatus(prio) != 0) {
        pLog->err(0, 0, "TASK DON'T EXEC : level %d", prio);
        return NULL;
    }
    t = &Task[prio];
    t->hook = TaskExec_hook;
    t->pFunc = (void (*)(int)) func;
    t->Status = TASK_EXEC;
    t->arg = arg;
    t->flag = 6;
    t->Priority = 0xF;
    return t;
}

// Called from a task: yields for `frames` frames — the scheduler thread resumes, the task thread
// sleeps on its queue until TaskSchedulerMain wakes it.
void TaskSleep(int ctr)
{
    if (ctr == 0) {
        return;
    }
    pCTask->SleepCtr = ctr;
    pCTask->Status = (pCTask->Status & TASK_SUSPEND) | TASK_SLEEP;
    if (pParentThread != NULL) {
        OSResumeThread(pParentThread);
    }
    if (pCTask->Priority > 0xF) {
        OSSignalSemaphore(&Sema);
    }
    OSSleepThread(&pCTask->Queue);
    if (pParentThread != NULL) {
        OSSuspendThread(pParentThread);
    }
    GXSetCurrentGXThread();
}

// Called from a task: replaces itself with `func(arg)` in the same slot (started next frame) and
// ends the current thread.
void TaskChain(TaskFunc func, int arg)
{
    pCTask->hook = TaskExec_hook;
    pCTask->pFunc = (void (*)(int)) func;
    pCTask->Status = TASK_EXEC;
    pCTask->arg = arg;
    if (pParentThread != NULL) {
        OSResumeThread(pParentThread);
    }
    if (pCTask->Priority > 0xF) {
        OSSignalSemaphore(&Sema);
    }
    OSExitThread(&pCTask->Thread);
}

// Called from a task: frees the slot and ends the thread (the scheduler thread resumes).
void TaskExit()
{
    TASK* t = pCTask;

    t->Status = TASK_NONE;
    t->suspend_cnt = 0;
    if (pParentThread != NULL) {
        OSResumeThread(pParentThread);
    }
    if (pCTask->Priority > 0xF) {
        OSSignalSemaphore(&Sema);
    }
    OSExitThread(&pCTask->Thread);
}

// Kills the task in slot `prio`.
void TaskKill(int prio)
{
    TaskKill(&Task[prio]);
}

// Kills a task: a sleeping / suspended thread is cancelled; a running one (the caller itself)
// exits; slot freed.
void TaskKill(TASK* t)
{
#ifdef RE4_PORT
    port_trace("[port] TaskKill slot %d: status %x, thread state %d, from thread %d\n", (int) (t - Task), t->Status, (int) t->Thread.state, sceKernelGetThreadId());
#endif
    if (t->Status == 0) {
        return;
    }
    switch (t->Status & ~TASK_SUSPEND) {
    case TASK_NONE:
    case TASK_EXEC:
        break;
    case TASK_SLEEP:
        OSCancelThread(&t->Thread);
        break;
    case TASK_RUN:
        if (t->Status & TASK_SUSPEND) {
            OSCancelThread(&t->Thread);
        } else {
#ifdef RE4_PORT
            // On the GameCube a running task means the caller is that task. On the PSP the task's
            // thread can be blocked in a disc read while another thread (the main loop) kills it:
            // cancel it instead of exiting the caller.
            if (OSGetCurrentThread() != &t->Thread) {
                OSCancelThread(&t->Thread);
                break;
            }
#endif
            TaskExit();
        }
        break;
    }
    t->Status = TASK_NONE;
    t->suspend_cnt = 0;
}

// Suspends slot `task` (counted; TASK_SUSPEND bit).
void TaskSuspend(int level)
{
    TASK* t = &Task[level];

    t->suspend_cnt++;
    t->Status |= TASK_SUSPEND;
}

// Undoes one TaskSuspend; the task runs again when the count reaches 0.
void TaskSignal(int level)
{
    TASK* t = &Task[level];

    if (t->suspend_cnt == 0) {
        return;
    }
    t->suspend_cnt--;
    if (t->suspend_cnt != 0) {
        return;
    }
    t->Status &= ~TASK_SUSPEND;
}

// Status of slot `prio` (0 = free).
u8 TaskStatus(int level)
{
    return Task[level].Status;
}

// The model a task works on (the current task when t is NULL) — read by the scenario / camera.
void SetTaskModelPtr(void* model, TASK* t)
{
    if (t == NULL) {
        pCTask->pModel = model;
    } else {
        t->pModel = model;
    }
}

// From the main loop after the CPU work of the frame (main.cpp): lets the background task slot
// (TASK_ISR) run in the time left before the vsync, when one is active.
void iTaskScheduler()
{
    BOOL lv = OSDisableInterrupts();

    if (iTask_exec_flg == 1) {
        TASK* t = &Task[TASK_ISR];
        pParentThread = NULL;
        pCTask = t;
        t->Status &= ~TASK_SUSPEND;
        TaskSchedulerMain(t);
        pCTask = CTASK_MAIN;
    }
    OSRestoreInterrupts(lv);
}

// Starts `func` in the interrupt-driven slot (decompression / loading in the background); only
// one at a time. Returns NULL when busy.
TASK* iTaskExec(TaskFunc func)
{
    TASK* t;
    int flg = iTask_exec_flg;

    if (flg == 0) {
        iTask_exec_flg = 1;
        t = TaskExec(TASK_ISR, func, 0);
        t->Priority = flg;
        return t;
    }
    return NULL;
}

// Kills the ISR task.
void iTaskKill()
{
    iTask_exec_flg = 0;
    TaskKill(&Task[TASK_ISR]);
}

// Called from the ISR task: ends it.
void iTaskExit()
{
    iTask_exec_flg = 0;
    TaskExit();
}

// From the VI retrace callback: suspends the background task's thread if it is still running so
// the main thread gets the CPU back for the next frame.
void iTaskSuspend()
{
    if (iTask_exec_flg == 1) {
        TASK* t = &Task[TASK_ISR];
#ifdef RE4_PORT
        static int nTr;
        if (++nTr <= 40) port_trace("[port] iTaskSuspend: status %x thread state %d\n", t->Status, (int) t->Thread.state);
#endif
        if (t->Thread.state == 2) {
            t->Status |= TASK_SUSPEND;
            OSSuspendThread(&t->Thread);
        }
    }
}

#ifdef RE4_PORT
// The background task's own check of the retrace suspension: sceKernelSuspendThread from the
// vblank thread does not hold a thread that is blocked in a kernel wait (a disc read) when the
// wait ends, so the task also stops itself here, at the safe points of its loops, until
// iTaskScheduler lets it run again. Its window is the end of the frame; outside it the main
// thread's own disc reads share the DVD layer's buffers with it.
void port_isr_checkpoint(void)
{
    TASK* t = &Task[TASK_ISR];
    if (OSGetCurrentThread() != &t->Thread) {  // (pCTask is the task the main thread schedules)
        return;
    }
    while ((t->Status & TASK_SUSPEND) && iTask_exec_flg == 1) {
        OSYieldThread();
    }
}
#endif

// 1 while an ISR task runs.
int iTaskStatus()
{
    return iTask_exec_flg;
}
