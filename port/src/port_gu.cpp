// port/src/port_gu: the PSP GE behind plain-typed wrappers (port_gu.h). Only this unit includes
// the pspsdk graphics headers.
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <pspdebug.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include "port_gu.h"

extern "C" void port_trace(const char* fmt, ...);
#define BUF_W 512
#define FRAME_BYTES (BUF_W * PG_SCREEN_H * 4)

static unsigned int __attribute__((aligned(16))) guList[512 * 1024 / 4];
// The vertices of a frame's draws (pg_get_memory): a ring the GE reads from directly, reset at
// pg_start (pg_finish waited for the previous frame's GE work). Kept out of the display list, whose
// 512 KB would overflow on a room's geometry.
#define VERT_RING_BYTES (2 * 1024 * 1024)
static unsigned char __attribute__((aligned(16))) vertRing[VERT_RING_BYTES];
static unsigned int vertUsed;
static unsigned int vertOverflow;  // draws refused this frame
static unsigned int vertMaxReq, vertFirstFail, vertFirstFailUsed;
static unsigned int vertWraps;     // ring restarts (GE stalls) of the stats period
static unsigned int maxListBytes;  // the largest display list of the last stats period
static unsigned int midFlushes;    // lists finished mid-frame because they were nearly full
static unsigned int usSync, usDraw;  // microseconds per stats period: waiting for the GE, issuing draws
// ms0:/PSP/GAME/RE4/port.txt, read once at pg_init: "dump N" lines schedule a frame dump (with the
// [draw] trace of every draw of that frame) at frame N; a debugger-free way to look at a frame.
static unsigned int dumpFrames[16];
static int nDumpFrames;
static void readPortConfig(void)
{
    int fd = sceIoOpen("ms0:/PSP/GAME/RE4/port.txt", PSP_O_RDONLY, 0);
    if (fd < 0) return;
    char buf[512];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = 0;
    const char* p = buf;
    while (*p && nDumpFrames < 16) {
        if (strncmp(p, "dump ", 5) == 0) {
            dumpFrames[nDumpFrames++] = (unsigned int) strtoul(p + 5, NULL, 10);
            port_trace("[ge] port.txt: dump at frame %u\n", dumpFrames[nDumpFrames - 1]);
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}
static void* drawBuf;              // the buffer the current list draws into (sceGuSwapBuffers' return)
static void* dispBuf = (void*) FRAME_BYTES;  // the buffer on screen
static char dumpPath[160];         // pg_request_dump: the finished frame is saved at the next pg_finish

extern "C" {

void pg_init(void)
{
    readPortConfig();
    sceGuInit();
    sceGuStart(GU_DIRECT, guList);
    sceGuDrawBuffer(GU_PSM_8888, (void*) 0, BUF_W);
    sceGuDispBuffer(PG_SCREEN_W, PG_SCREEN_H, (void*) FRAME_BYTES, BUF_W);
    sceGuDepthBuffer((void*) (FRAME_BYTES * 2), BUF_W);
    sceGuOffset(2048 - PG_SCREEN_W / 2, 2048 - PG_SCREEN_H / 2);
    sceGuViewport(2048, 2048, PG_SCREEN_W, PG_SCREEN_H);
    sceGuDepthRange(0, 65535);
    sceGuScissor(0, 0, PG_SCREEN_W, PG_SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_LEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CCW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuDisable(GU_LIGHTING);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuClearColor(0xFF000000);
    sceGuClearDepth(65535);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    sceGuStart(GU_DIRECT, guList);
}

extern int pg_stat_draws, pg_stat_verts, pg_stat_clears;  // per frame (below)
void pg_start(void)
{
    static unsigned int frames, lastTime, sumDraws, sumVerts;
    frames++;
    sumDraws += pg_stat_draws;
    sumVerts += pg_stat_verts;
    if ((frames & 127) == 0) {  // every 128 frames: the frame rate and the average draw load
        unsigned int now = sceKernelGetSystemTimeLow();
        extern unsigned int port_gx_texcache_bytes(void);
        struct mallinfo mi = mallinfo();
        extern unsigned int port_gx_stat_conversions, port_gx_stat_copies, port_gx_stat_dropped;
        port_trace("[ge] frame %u: %u ms/frame, %u draws, %u vertices, %u texture conversions, %u frame copies per frame, list up to %u KB; heap %u KB used %u KB free, textures %u KB, stack left %d KB\n", frames, (now - lastTime) / 1000 / 128, sumDraws / 128, sumVerts / 128,
                   port_gx_stat_conversions / 128, port_gx_stat_copies / 128, maxListBytes / 1024, (unsigned) mi.uordblks / 1024, (unsigned) mi.fordblks / 1024, port_gx_texcache_bytes() / 1024, sceKernelCheckThreadStack() / 1024);
        {
            extern unsigned int port_gx_us_xform, port_gx_us_state;
            port_trace("[ge] time per frame: %u ms vertex transform, %u ms draw state, %u ms draw issue, %u ms GE wait\n",
                       port_gx_us_xform / 1000 / 128, port_gx_us_state / 1000 / 128, usDraw / 1000 / 128, usSync / 1000 / 128);
            port_gx_us_xform = port_gx_us_state = usDraw = usSync = 0;
        }
        if (midFlushes) port_trace("[ge] %u display lists finished mid-frame (%u vertex ring wraps)\n", midFlushes, vertWraps);
        vertWraps = 0;
        if (port_gx_stat_dropped) port_trace("[ge] %u bad-vertex draws dropped per frame\n", port_gx_stat_dropped / 128);
        port_gx_stat_conversions = port_gx_stat_copies = port_gx_stat_dropped = 0;
        maxListBytes = 0;
        midFlushes = 0;
        lastTime = now;
        sumDraws = sumVerts = 0;
    }
    pg_stat_draws = pg_stat_verts = pg_stat_clears = 0;
    for (int i = 0; i < nDumpFrames; i++) {  // the scheduled dumps of ms0:/PSP/GAME/RE4/port.txt ("dump N" lines)
        if (dumpFrames[i] == frames && !dumpPath[0]) {
            char path[64];
            sprintf(path, "ms0:/PSP/GAME/RE4/dump%u.raw", frames);
            port_trace("[ge] frame %u: scheduled dump\n", frames);
            pg_request_dump(path);
        }
    }
    if (vertOverflow) {
        port_trace("[ge] %u draws did not fit the %u KB vertex ring last frame (largest request %u B; first refused %u B at %u KB used)\n", vertOverflow, VERT_RING_BYTES / 1024, vertMaxReq, vertFirstFail, vertFirstFailUsed / 1024);
        vertOverflow = 0;
    }
    vertUsed = 0;
    vertMaxReq = 0;
    sceGuStart(GU_DIRECT, guList);
    // sceGuStart only emits the frame buffer address when it is not 0; buffer 0 needs it explicitly
    sceGuDrawBuffer(GU_PSM_8888, drawBuf, BUF_W);
}

static void saveBuffer(const char* path, const void* buf);
void pg_finish(void)
{
    unsigned int n = (unsigned int) sceGuFinish();  // the list's size in bytes
    if (n > maxListBytes) maxListBytes = n;
    if (n > sizeof(guList) - 4096) port_trace("[ge] display list %u bytes: at the 512 KB buffer's end\n", n);
    unsigned int t0 = sceKernelGetSystemTimeLow();
    sceGuSync(0, 0);
    usSync += sceKernelGetSystemTimeLow() - t0;
    if (dumpPath[0]) {  // the list just drew the whole frame into drawBuf
        for (int i = 0; i < 256; i += 8) {
            port_trace("[ge] %08x %08x %08x %08x %08x %08x %08x %08x\n", guList[i], guList[i + 1], guList[i + 2], guList[i + 3], guList[i + 4], guList[i + 5], guList[i + 6], guList[i + 7]);
        }
        saveBuffer(dumpPath, (const unsigned char*) 0x44000000 + (unsigned int) drawBuf);
        port_trace("[ge] frame dumped from draw buffer %p to %s\n", drawBuf, dumpPath);
        dumpPath[0] = 0;
    }
}
void pg_request_dump(const char* path)
{
    strncpy(dumpPath, path, sizeof(dumpPath) - 1);
}
int pg_dump_pending(void) { return dumpPath[0] != 0; }  // port_gx traces its draws into the log meanwhile

static int swaps;
int pg_stat_draws, pg_stat_verts, pg_stat_clears;  // per frame, for the demo's log
void pg_swap(void)
{
    dispBuf = drawBuf;
    drawBuf = sceGuSwapBuffers();
    swaps++;
}

void pg_clear(unsigned int color, unsigned int depth, int colorToo, int depthToo)
{
    pg_stat_clears++;
    sceGuClearColor(color);
    sceGuClearDepth(depth);
    sceGuClear((colorToo ? GU_COLOR_BUFFER_BIT : 0) | (depthToo ? GU_DEPTH_BUFFER_BIT : 0));
}

void pg_viewport(float cx, float cy, float w, float h)
{
    sceGuViewport((int) cx, (int) cy, (int) w, (int) h);
}

void pg_scissor(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > PG_SCREEN_W) w = PG_SCREEN_W - x;
    if (y + h > PG_SCREEN_H) h = PG_SCREEN_H - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    sceGuScissor(x, y, w, h);
}

void pg_depth(int testEnable, int func, int writeEnable)
{
    if (testEnable) {
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthFunc(func);
    } else {
        sceGuDisable(GU_DEPTH_TEST);
    }
    sceGuDepthMask(writeEnable ? GU_FALSE : GU_TRUE);
}

void pg_blend(int enable, int op, int src, int dst, unsigned int fixSrc, unsigned int fixDst)
{
    if (enable) {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(op, src, dst, fixSrc, fixDst);
    } else {
        sceGuDisable(GU_BLEND);
    }
}

void pg_alpha_test(int enable, int func, int ref)
{
    if (enable) {
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(func, ref, 0xFF);
    } else {
        sceGuDisable(GU_ALPHA_TEST);
    }
}

void pg_cull(int enable, int frontCW)
{
    if (enable) {
        sceGuEnable(GU_CULL_FACE);
        sceGuFrontFace(frontCW ? GU_CW : GU_CCW);
    } else {
        sceGuDisable(GU_CULL_FACE);
    }
}

void pg_pixel_mask(unsigned int mask) { sceGuPixelMask(mask); }

void pg_fog(int enable, float nearz, float farz, unsigned int color)
{
    if (enable) {
        sceGuEnable(GU_FOG);
        sceGuFog(nearz, farz, color);
    } else {
        sceGuDisable(GU_FOG);
    }
}

void pg_texture_off(void) { sceGuDisable(GU_TEXTURE_2D); }

void pg_texture(int psm, int w, int h, const void* data, int wrapS, int wrapT, int minFilt, int magFilt, int tfx, int tcc, float su, float sv, const unsigned int* clut, int clutEntries)
{
    sceGuEnable(GU_TEXTURE_2D);
    if (clut) {
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad(clutEntries / 8, clut);
    }
    sceGuTexMode(psm, 0, 0, 0);
    sceGuTexImage(0, w, h, w, data);
    sceGuTexFilter(minFilt, magFilt);
    sceGuTexWrap(wrapS, wrapT);
    sceGuTexFunc(tfx, tcc ? GU_TCC_RGBA : GU_TCC_RGB);
    sceGuTexScale(su, sv);  // the image may be padded to a power of two (port_gx fitTexture)
    sceGuTexOffset(0.0f, 0.0f);
}

void pg_projection(const float* m16)
{
    ScePspFMatrix4 p;
    float* d = (float*) &p;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            d[c * 4 + r] = m16[r * 4 + c];  // ScePspFMatrix4 is column vectors
        }
    }
    sceGuSetMatrix(GU_PROJECTION, &p);
    ScePspFMatrix4 id;
    memset(&id, 0, sizeof(id));
    id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
    sceGuSetMatrix(GU_VIEW, &id);
    sceGuSetMatrix(GU_MODEL, &id);
}

// libpspgu's current list (guInternal.h): the write pointer, to keep the list inside its buffer.
typedef struct { unsigned int* start; unsigned int* current; int parent_context; } PgGuDisplayList;
extern "C" PgGuDisplayList* gu_list;
static void listRoomCheck(void)
{
    if (gu_list && (unsigned int) ((char*) gu_list->current - (char*) gu_list->start) > sizeof(guList) - 16384) {
        // nearly full: hand it to the GE, wait, and start a new one in the same buffer (a room's
        // draws did not fit 512 KB; the GE's state carries over between lists)
        unsigned int n = (unsigned int) sceGuFinish();
        if (n > maxListBytes) maxListBytes = n;
        sceGuSync(0, 0);
        sceGuStart(GU_DIRECT, guList);
        sceGuDrawBuffer(GU_PSM_8888, drawBuf, BUF_W);
        midFlushes++;
    }
}

void pg_draw(int prim, int count, const PgVertex* verts)
{
    if (count <= 0 || verts == NULL) return;
    unsigned int t0 = sceKernelGetSystemTimeLow();
    listRoomCheck();
    pg_stat_draws++;
    pg_stat_verts += count;
    sceKernelDcacheWritebackRange(verts, count * sizeof(PgVertex));  // the CPU wrote them; the GE reads memory
    sceGuDrawArray(prim, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, count, 0, verts);
    usDraw += sceKernelGetSystemTimeLow() - t0;
}

void* pg_get_memory(int bytes)
{
    unsigned int n = ((unsigned int) bytes + 15) & ~15u;
    if (vertUsed + n > VERT_RING_BYTES) {
        if (n > VERT_RING_BYTES) {
            if (!vertOverflow) vertFirstFail = n, vertFirstFailUsed = vertUsed;
            vertOverflow++;
            return NULL;
        }
        // the ring is full: let the GE finish what it has (its vertices live in the ring) and reuse
        // the ring from the start; a stall, but the frame is complete. Rooms submit ~2 MB of
        // vertices per frame in this format.
        unsigned int done = (unsigned int) sceGuFinish();
        if (done > maxListBytes) maxListBytes = done;
        sceGuSync(0, 0);
        sceGuStart(GU_DIRECT, guList);
        sceGuDrawBuffer(GU_PSM_8888, drawBuf, BUF_W);
        midFlushes++;
        vertWraps++;
        vertUsed = 0;
    }
    if (n > vertMaxReq) vertMaxReq = n;
    void* p = vertRing + vertUsed;
    vertUsed += n;
    return p;
}

static int overlayInit;
void pg_overlay_begin(void)
{
    void* shown = (void*) (0x44000000 + (unsigned int) dispBuf);
    if (!overlayInit) {
        pspDebugScreenInitEx(shown, PSP_DISPLAY_PIXEL_FORMAT_8888, 0);
        overlayInit = 1;
    }
    pspDebugScreenSetBase((u32*) shown);
    pspDebugScreenSetTextColor(0xFF40FF40);
}

void pg_debug_print(int col, int row, unsigned int color, const char* text)
{
    pspDebugScreenSetXY(col, row);
    pspDebugScreenSetTextColor(color);
    pspDebugScreenPuts(text);
}

void pg_dcache_writeback(const void* p, int bytes) { sceKernelDcacheWritebackRange(p, bytes); }

void* pg_uncached(void* p) { return (void*) ((unsigned int) p | 0x40000000); }

void pg_wait_vblank(void) { sceDisplayWaitVblankStart(); }

static void saveBuffer(const char* path, const void* buf)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd < 0) return;
    for (int y = 0; y < PG_SCREEN_H; y++) {
        sceIoWrite(fd, (const unsigned char*) buf + y * BUF_W * 4, PG_SCREEN_W * 4);
    }
    sceIoClose(fd);
}

// Saves both frame buffers: "<path>" is the one on screen, "<path>.draw" the one being drawn.
int pg_save_frame(const char* path)
{
    sceGuFinish();
    sceGuSync(0, 0);
    for (int i = 0; i < 256; i += 8) {
        port_trace("[ge] %08x %08x %08x %08x %08x %08x %08x %08x\n", guList[i], guList[i + 1], guList[i + 2], guList[i + 3], guList[i + 4], guList[i + 5], guList[i + 6], guList[i + 7]);
    }
    void* shown = (void*) (0x44000000 + (unsigned int) dispBuf);
    void* drawn = (void*) (0x44000000 + (unsigned int) drawBuf);
    saveBuffer(path, shown);
    char p2[160];
    strcpy(p2, path);
    strcat(p2, ".draw");
    saveBuffer(p2, drawn);
    sceGuStart(GU_DIRECT, guList);
    return 1;
}

void pg_copy_frame(unsigned int* dst, int dstW, int dstH, int srcX, int srcY, int srcW, int srcH, int mode)
{
    // the list so far must have drawn before the CPU reads the buffer; then the list restarts
    sceGuFinish();
    sceGuSync(0, 0);
    const unsigned int* color = (const unsigned int*) (0x44000000 + (unsigned int) drawBuf);
    const unsigned short* depth = (const unsigned short*) (0x44000000 + FRAME_BYTES * 2);
    if (srcW < 1) srcW = 1;
    if (srcH < 1) srcH = 1;
    for (int y = 0; y < dstH; y++) {
        int sy = srcY + y * srcH / dstH;
        if (sy < 0) sy = 0;
        if (sy >= PG_SCREEN_H) sy = PG_SCREEN_H - 1;
        for (int x = 0; x < dstW; x++) {
            int sx = srcX + x * srcW / dstW;
            if (sx < 0) sx = 0;
            if (sx >= PG_SCREEN_W) sx = PG_SCREEN_W - 1;
            unsigned int v;
            if (mode == 2) {
                unsigned int z = depth[sy * BUF_W + sx] >> 8;
                v = 0xFF000000u | (z << 16) | (z << 8) | z;
            } else {
                v = color[sy * BUF_W + sx];
                if (mode == 1) {
                    unsigned int a = v >> 24;
                    v = (a << 24) | (a << 16) | (a << 8) | a;
                }
            }
            dst[y * dstW + x] = v;
        }
    }
    sceKernelDcacheWritebackRange(dst, dstW * dstH * 4);
    sceGuStart(GU_DIRECT, guList);
}

}  // extern "C"
