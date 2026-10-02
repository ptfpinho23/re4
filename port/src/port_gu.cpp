// port/src/port_gu: the PSP GE behind plain-typed wrappers (port_gu.h). Only this unit includes
// the pspsdk graphics headers.
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <pspdebug.h>
#include <psppower.h>
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
static int nativeModelValid;       // the GE holds a native draw's model matrix (pg_draw_native)
extern "C" volatile unsigned int port_gx_flags;  // port_gx.cpp: the debugger's toggles
static unsigned int usSync, usDraw;  // microseconds per stats period: waiting for the GE, issuing draws
// ms0:/PSP/GAME/RE4/port.txt, read once at pg_init: "dump N" lines schedule a frame dump (with the
// [draw] trace of every draw of that frame) at frame N; a debugger-free way to look at a frame.
extern "C" { volatile int port_dump_now; }  // set from the debugger: dump the next frame
static unsigned int dumpFrames[16];
extern "C" { volatile int port_profile; }  // port.txt "profile" (or a debugger poke): time the draw phases (two system calls per phase per draw)
static inline unsigned int usProfile(void) { return port_profile ? sceKernelGetSystemTimeLow() : 0; }
extern "C" { int port_overlay_on; }  // port.txt "overlay": draw the port's log lines on the screen (off: they are in port.log)
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
        } else if (strncmp(p, "overlay", 7) == 0) {
            port_overlay_on = 1;  // the port's console text on the screen: it lands in the game's frame (and its alpha, which the filters read)
        } else if (strncmp(p, "profile", 7) == 0) {
            port_profile = 1;
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
    scePowerSetClockFrequency(333, 333, 166);  // the PSP starts at 222 MHz
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
    sceGuTexOffset(0.0f, 0.0f);
    sceGuModelColor(0, 0xFFFFFF, 0xFFFFFF, 0);  // (emissive and specular stay black)
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
        port_trace("[ge] frame %u: %u ms/frame, %u draws, %u vertices, %u texture conversions per 128 frames, %u frame copies per frame, list up to %u KB; heap %u KB used %u KB free, textures %u KB, stack left %d KB\n", frames, (now - lastTime) / 1000 / 128, sumDraws / 128, sumVerts / 128,
                   port_gx_stat_conversions, port_gx_stat_copies / 128, maxListBytes / 1024, (unsigned) mi.uordblks / 1024, (unsigned) mi.fordblks / 1024, port_gx_texcache_bytes() / 1024, sceKernelCheckThreadStack() / 1024);
        port_trace("[ge] memory: %u KB free in the system, largest block %u KB\n", (unsigned) sceKernelTotalFreeMemSize() / 1024, (unsigned) sceKernelMaxFreeMemSize() / 1024);
        {
            extern unsigned int port_gx_us_xform, port_gx_us_state, port_gx_stat_native, port_gx_stat_cpu, port_gx_stat_why[8];
            port_trace("[ge] time per frame: %u ms vertices, %u ms draw state, %u ms draw issue, %u ms GE wait; vertices per frame: %u native, %u cpu\n",
                       port_gx_us_xform / 1000 / 128, port_gx_us_state / 1000 / 128, usDraw / 1000 / 128, usSync / 1000 / 128, port_gx_stat_native / 128, port_gx_stat_cpu / 128);
            port_trace("[ge] cpu-path vertices per frame by reason: prim %u, matrix index %u, pos/colour format %u, normal %u, texgen %u, lighting model %u, spot angle %u, >4 lights %u\n",
                       port_gx_stat_why[0] / 128, port_gx_stat_why[1] / 128, port_gx_stat_why[2] / 128, port_gx_stat_why[3] / 128, port_gx_stat_why[4] / 128, port_gx_stat_why[5] / 128,
                       port_gx_stat_why[6] / 128, port_gx_stat_why[7] / 128);
            extern unsigned int port_gx_stat_flat;
            port_trace("[ge] native path: %u draws per frame with a fifth light folded into the ambient\n", port_gx_stat_flat / 128);
            port_gx_stat_flat = 0;
            extern unsigned int port_gx_stat_baked, port_gx_stat_bakes, port_gx_stat_transient, port_gx_stat_baked_calls;
            extern unsigned int port_gx_bake_bytes(void);
            port_trace("[ge] display lists: %u calls per frame drawn whole, %u vertices from bakes, %u gathered for the call; %u bakes per 128 frames, %u KB baked\n",
                       port_gx_stat_baked_calls / 128, port_gx_stat_baked / 128, port_gx_stat_transient / 128, port_gx_stat_bakes, port_gx_bake_bytes() / 1024);
            port_gx_stat_baked = port_gx_stat_bakes = port_gx_stat_transient = port_gx_stat_baked_calls = 0;
            extern unsigned int port_gx_stat_walk[6];
            port_trace("[ge] display lists walked per frame: %u remembered, %u no draw first, %u format, %u colours, %u not plain draws, %u lights\n",
                       port_gx_stat_walk[0] / 128, port_gx_stat_walk[1] / 128, port_gx_stat_walk[2] / 128, port_gx_stat_walk[3] / 128, port_gx_stat_walk[4] / 128, port_gx_stat_walk[5] / 128);
            for (int k = 0; k < 6; k++) port_gx_stat_walk[k] = 0;
            extern unsigned int port_gx_us_gather, port_gx_us_native, port_gx_stat_native_draws, port_gx_stat_cpu_draws;
            port_trace("[ge] native path: %u draws, %u ms in all, %u ms gathering; cpu path: %u draws per frame\n", port_gx_stat_native_draws / 128, port_gx_us_native / 1000 / 128, port_gx_us_gather / 1000 / 128, port_gx_stat_cpu_draws / 128);
            port_gx_us_gather = port_gx_us_native = port_gx_stat_native_draws = port_gx_stat_cpu_draws = 0;
            for (int k = 0; k < 8; k++) port_gx_stat_why[k] = 0;
            port_gx_us_xform = port_gx_us_state = usDraw = usSync = port_gx_stat_native = port_gx_stat_cpu = 0;
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
static unsigned int dumpFinishes;  // lists finished since the last swap
void pg_finish(void)
{
    unsigned int n = (unsigned int) sceGuFinish();  // the list's size in bytes
    if (n > maxListBytes) maxListBytes = n;
    if (n > sizeof(guList) - 4096) port_trace("[ge] display list %u bytes: at the 512 KB buffer's end\n", n);
    unsigned int t0 = sceKernelGetSystemTimeLow();
    sceGuSync(0, 0);
    usSync += sceKernelGetSystemTimeLow() - t0;
    if (dumpPath[0]) port_trace("[ge] list %u finished (%u bytes)\n", dumpFinishes, n);
    dumpFinishes++;
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
    if (dumpPath[0]) {  // the frame (all of its lists: a room draws several) is complete in drawBuf
        saveBuffer(dumpPath, (const unsigned char*) 0x44000000 + (unsigned int) drawBuf);
        port_trace("[ge] frame dumped from draw buffer %p to %s (%u lists)\n", drawBuf, dumpPath, dumpFinishes);
        dumpPath[0] = 0;
    }
    dumpFinishes = 0;
    dispBuf = drawBuf;
    drawBuf = sceGuSwapBuffers();
    pg_texture_forget();  // (once a frame: whatever was missed does not outlive it)
    swaps++;
    if (port_dump_now) {  // a debugger poke (tools/port/ppsspp.py: poke port_dump_now 01000000): dump the frame that starts now
        port_dump_now = 0;
        if (!dumpPath[0]) {
            char path[64];
            sprintf(path, "ms0:/PSP/GAME/RE4/dump%u.raw", (unsigned) swaps);
            port_trace("[ge] swap %u: dump requested by the debugger\n", (unsigned) swaps);
            pg_request_dump(path);
        }
    }
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
    if (dumpPath[0]) port_trace("[ge] state: depth test %d func %d write %d (draw %d)\n", testEnable, func, writeEnable, pg_stat_draws);
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
    if (dumpPath[0]) port_trace("[ge] state: blend %d op %d %d %d fix %06x %06x (draw %d)\n", enable, op, src, dst, fixSrc, fixDst, pg_stat_draws);
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

void pg_pixel_mask(unsigned int mask)
{
    if (dumpPath[0]) port_trace("[ge] state: pixel mask %08x (draw %d)\n", mask, pg_stat_draws);
    sceGuPixelMask(mask);
}

void pg_fog(int enable, float nearz, float farz, unsigned int color)
{
    if (enable) {
        sceGuEnable(GU_FOG);
        sceGuFog(nearz, farz, color);
    } else {
        sceGuDisable(GU_FOG);
    }
}

// What the GE's texture unit holds: a draw that changes nothing sends nothing (a room is thousands
// of draws a frame, most with the texture of the one before).
static struct {
    int on;  // -1 unknown
    int psm, w, h, wrapS, wrapT, minFilt, magFilt, tfx, tcc, clutEntries;
    const void* data;
    const unsigned int* clut;
    float su, sv;
} geTex = {-1, -1, 0, 0, -1, -1, -1, -1, -1, -1, 0, NULL, NULL, 0.0f, 0.0f};

void pg_texture_off(void)
{
    if (geTex.on != 0) { sceGuDisable(GU_TEXTURE_2D); geTex.on = 0; }
}

// The texels or palette behind a pointer changed (a conversion was freed, a frame copy redrawn):
// the next bind sends the image again, which also flushes the GE's texture cache.
void pg_texture_forget(void)
{
    geTex.data = NULL;
    geTex.clut = NULL;
}

static int lastTexPsm, lastTexW, lastTexH, lastTexTfx, lastTexTcc, lastTexOn;
static const void* lastTexData;
static float lastTexSu, lastTexSv;
void pg_texture(int psm, int w, int h, const void* data, int wrapS, int wrapT, int minFilt, int magFilt, int tfx, int tcc, float su, float sv, const unsigned int* clut, int clutEntries)
{
    lastTexPsm = psm; lastTexW = w; lastTexH = h; lastTexData = data; lastTexTfx = tfx; lastTexTcc = tcc; lastTexSu = su; lastTexSv = sv; lastTexOn = 1;
    if (geTex.on != 1) { sceGuEnable(GU_TEXTURE_2D); geTex.on = 1; }
    if (clut && (clut != geTex.clut || clutEntries != geTex.clutEntries)) {
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad(clutEntries / 8, clut);
        geTex.clut = clut; geTex.clutEntries = clutEntries;
    }
    if (data != geTex.data || psm != geTex.psm || w != geTex.w || h != geTex.h) {
        sceGuTexMode(psm, 0, 0, 0);
        sceGuTexImage(0, w, h, w, data);
        geTex.data = data; geTex.psm = psm; geTex.w = w; geTex.h = h;
    }
    if (minFilt != geTex.minFilt || magFilt != geTex.magFilt) {
        sceGuTexFilter(minFilt, magFilt);
        geTex.minFilt = minFilt; geTex.magFilt = magFilt;
    }
    if (wrapS != geTex.wrapS || wrapT != geTex.wrapT) {
        sceGuTexWrap(wrapS, wrapT);
        geTex.wrapS = wrapS; geTex.wrapT = wrapT;
    }
    if (tfx != geTex.tfx || tcc != geTex.tcc) {
        sceGuTexFunc(tfx, tcc ? GU_TCC_RGBA : GU_TCC_RGB);
        geTex.tfx = tfx; geTex.tcc = tcc;
    }
    if (su != geTex.su || sv != geTex.sv) {
        sceGuTexScale(su, sv);  // the image may be padded to a power of two (port_gx fitTexture)
        geTex.su = su; geTex.sv = sv;
    }
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
    nativeModelValid = 0;  // (declared below: a native draw reloads its model matrix)
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

// ---- native draws: the GE transforms, lights and maps the texture coordinates
static int nativeActive;         // the GE's model matrix / lighting / texture map mode are a native draw's
static int nativeLit, nativeTexMatrix;
static float nativeModel[12];
static unsigned int nativeLightSig, nativeAmbient;
static int geColorMaterial = -1;
static unsigned int geMatAmbient = 0x12345678u, geMatDiffuse = 0xFF000000u, geEmissive;  // (values no draw asks for: sent on first use)
static void leaveNative(void)
{
    ScePspFMatrix4 id;
    memset(&id, 0, sizeof(id));
    id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
    sceGuSetMatrix(GU_MODEL, &id);
    if (nativeLit) sceGuDisable(GU_LIGHTING);
    if (nativeTexMatrix) sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
    nativeLit = nativeTexMatrix = nativeModelValid = nativeActive = 0;
}

void pg_draw_native(int prim, int count, const void* verts, int bytes, const PgNative* n)
{
    if (count <= 0 || verts == NULL) return;
    unsigned int t0 = usProfile();
    listRoomCheck();
    pg_stat_draws++;
    pg_stat_verts += count;
    if (!nativeModelValid || memcmp(nativeModel, n->model, sizeof(nativeModel)) != 0) {
        ScePspFMatrix4 m;  // column vectors: x / y / z are the matrix's columns, w the translation
        const float* r = n->model;
        m.x.x = r[0]; m.x.y = r[4]; m.x.z = r[8];  m.x.w = 0.0f;
        m.y.x = r[1]; m.y.y = r[5]; m.y.z = r[9];  m.y.w = 0.0f;
        m.z.x = r[2]; m.z.y = r[6]; m.z.z = r[10]; m.z.w = 0.0f;
        m.w.x = r[3]; m.w.y = r[7]; m.w.z = r[11]; m.w.w = 1.0f;
        sceGuSetMatrix(GU_MODEL, &m);
        memcpy(nativeModel, n->model, sizeof(nativeModel));
        nativeModelValid = 1;
    }
    if (n->texMatrix) {
        ScePspFMatrix4 t;  // (s, t, q) = (a u + b v + c, d u + e v + f, 1): the GE divides by q
        memset(&t, 0, sizeof(t));
        t.x.x = n->texMtx[0]; t.x.y = n->texMtx[3];
        t.y.x = n->texMtx[1]; t.y.y = n->texMtx[4];
        t.w.x = n->texMtx[2]; t.w.y = n->texMtx[5]; t.w.z = 1.0f; t.w.w = 1.0f;
        sceGuSetMatrix(GU_TEXTURE, &t);
        if (!nativeTexMatrix) {
            sceGuTexMapMode(GU_TEXTURE_MATRIX, 0, 0);
            sceGuTexProjMapMode(GU_UV);
            nativeTexMatrix = 1;
        }
    } else {
        if (nativeTexMatrix) { sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0); nativeTexMatrix = 0; }
        // (the scale is the bound texture's: pg_texture sent it)
    }
    if (n->lit) {
        if (!nativeLit || nativeLightSig != n->lightSig) {
            if (!nativeLit) sceGuEnable(GU_LIGHTING);
            for (int i = 0; i < 4; i++) {
                if (i < n->nLights) {
                    const PgNativeLight* l = &n->light[i];
                    ScePspFVector3 p = {l->pos[0], l->pos[1], l->pos[2]};
                    sceGuEnable(GU_LIGHT0 + i);
                    sceGuLight(i, l->spot ? GU_SPOTLIGHT : GU_POINTLIGHT, GU_AMBIENT_AND_DIFFUSE, &p);
                    if (l->spot) {
                        // the GE measures the angle between its direction and the vertex-to-light
                        // vector: the axis reversed
                        ScePspFVector3 d = {-l->dir[0], -l->dir[1], -l->dir[2]};
                        sceGuLightSpot(i, &d, l->exponent, l->cutoff);
                    }
                    sceGuLightColor(i, GU_DIFFUSE, l->ambientOnly ? 0 : l->color);
                    sceGuLightColor(i, GU_AMBIENT, l->ambientOnly ? l->color : 0);
                    sceGuLightColor(i, GU_SPECULAR, 0);
                    sceGuLightAtt(i, l->att[0], l->att[1], l->att[2]);
                } else {
                    sceGuDisable(GU_LIGHT0 + i);
                }
            }
            sceGuAmbient(n->ambient);
            nativeAmbient = n->ambient;
            nativeLit = 1;
            nativeLightSig = n->lightSig;
        }
        if (nativeAmbient != n->ambient) {  // (not part of the signature: faint lights are folded into it per draw)
            sceGuAmbient(n->ambient);
            nativeAmbient = n->ambient;
        }
    } else if (nativeLit) {
        sceGuDisable(GU_LIGHTING);
        nativeLit = 0;
    }
    {
        // the materials: the vertex colour where the game's channel takes it, else the registers
        // (a vertex without a colour is drawn in the ambient material's, lit or not)
        const int hasColor = (n->vtype & PG_VT_COLOR_8888) != 0;
        const int cm = hasColor && n->lit ? n->colorMaterial : 0;
        if (n->lit && cm != geColorMaterial) { sceGuColorMaterial(cm); geColorMaterial = cm; }
        if (n->lit && (n->emissive & 0xFFFFFF) != geEmissive) { sceGuSendCommandi(84, (int) (n->emissive & 0xFFFFFF)); geEmissive = n->emissive & 0xFFFFFF; }  // (the emissive material colour)
        if ((!hasColor || (n->lit && !(cm & 1))) && n->matAmbient != geMatAmbient) { sceGuMaterial(1, n->matAmbient); geMatAmbient = n->matAmbient; }
        if (n->lit && !(cm & 2) && (n->matDiffuse & 0xFFFFFF) != geMatDiffuse) { sceGuMaterial(2, n->matDiffuse); geMatDiffuse = n->matDiffuse & 0xFFFFFF; }
    }
    nativeActive = 1;
    if (dumpPath[0]) port_trace("[ge] native draw prim %d count %d vtype %x lit %d lights %d\n", prim, count, n->vtype, n->lit, n->nLights);
    if (bytes) sceKernelDcacheWritebackRange(verts, bytes);
    sceGuDrawArray(prim, n->vtype | GU_TRANSFORM_3D, count, 0, verts);
    usDraw += usProfile() - t0;
}

void pg_draw(int prim, int count, const PgVertex* verts)
{
    if (count <= 0 || verts == NULL) return;
    if (nativeActive) leaveNative();
    unsigned int t0 = usProfile();
    if (dumpPath[0]) port_trace("[ge] draw prim %d count %d\n", prim, count);
    if ((port_gx_flags & 8192) && prim == PG_TRIANGLES && count == 6) return;  // debugger: quads' state applied, pixels not drawn
    if ((port_gx_flags & 0x100000) && prim == PG_TRIANGLES && count == 6 && (lastTexPsm == PG_PSM_T8 || lastTexPsm == PG_PSM_T4)) {  // debugger: what the GE gets for a sprite quad
        static int nTr;
        if (++nTr <= 16) {
            port_trace("[ge] quad: tex %s psm %d %dx%d data %p tfx %d tcc %d scale %g %g\n", lastTexOn ? "on" : "off", lastTexPsm, lastTexW, lastTexH, lastTexData, lastTexTfx, lastTexTcc, lastTexSu, lastTexSv);
            for (int i = 0; i < 6; i++) port_trace("[ge]   v%d %g %g %g uv %g %g col %08x\n", i, verts[i].x, verts[i].y, verts[i].z, verts[i].u, verts[i].v, verts[i].color);
        }
    }
    listRoomCheck();
    pg_stat_draws++;
    pg_stat_verts += count;
    sceKernelDcacheWritebackRange(verts, count * sizeof(PgVertex));  // the CPU wrote them; the GE reads memory
    sceGuDrawArray(prim, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, count, 0, verts);
    usDraw += usProfile() - t0;
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
    if (port_gx_flags & 4) return;  // debugger: no frame copies (the copy textures keep their last content)
    // the list so far must have drawn before the CPU reads the buffer; then the list restarts
    sceGuFinish();
    sceGuSync(0, 0);
    const unsigned int* color = (const unsigned int*) (0x44000000 + (unsigned int) drawBuf);
    const unsigned short* depth = (const unsigned short*) (0x44000000 + FRAME_BYTES * 2);
    if (srcW < 1) srcW = 1;
    if (srcH < 1) srcH = 1;
    // A copy that shrinks the frame averages the source box of every destination pixel (the
    // GameCube's copy filter does): the filters build blur pyramids from these, and a
    // point-sampled one turns into speckles when it is stretched back over the screen.
    for (int y = 0; y < dstH; y++) {
        int sy0 = srcY + y * srcH / dstH, sy1 = srcY + (y + 1) * srcH / dstH;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy0 < 0) sy0 = 0;
        if (sy1 > PG_SCREEN_H) sy1 = PG_SCREEN_H;
        if (sy0 >= PG_SCREEN_H) sy0 = PG_SCREEN_H - 1;
        for (int x = 0; x < dstW; x++) {
            int sx0 = srcX + x * srcW / dstW, sx1 = srcX + (x + 1) * srcW / dstW;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx0 < 0) sx0 = 0;
            if (sx1 > PG_SCREEN_W) sx1 = PG_SCREEN_W;
            if (sx0 >= PG_SCREEN_W) sx0 = PG_SCREEN_W - 1;
            unsigned int v;
            if (sx1 - sx0 <= 1 && sy1 - sy0 <= 1) {
                if (mode == 2) {
                    unsigned int z = depth[sy0 * BUF_W + sx0] >> 8;
                    v = 0xFF000000u | (z << 16) | (z << 8) | z;
                } else {
                    v = color[sy0 * BUF_W + sx0];
                    if (mode == 1) {
                        unsigned int a = v >> 24;
                        v = (a << 24) | (a << 16) | (a << 8) | a;
                    }
                }
            } else {
                unsigned int r = 0, g = 0, b = 0, a = 0, n = 0;
                for (int yy = sy0; yy < sy1; yy++) {
                    for (int xx = sx0; xx < sx1; xx++) {
                        if (mode == 2) {
                            unsigned int z = depth[yy * BUF_W + xx] >> 8;
                            r += z; g += z; b += z; a += 255;
                        } else {
                            unsigned int c = color[yy * BUF_W + xx];
                            if (mode == 1) { unsigned int al = c >> 24; r += al; g += al; b += al; a += al; }
                            else { r += c & 255; g += (c >> 8) & 255; b += (c >> 16) & 255; a += c >> 24; }
                        }
                        n++;
                    }
                }
                v = ((a / n) << 24) | ((b / n) << 16) | ((g / n) << 8) | (r / n);
            }
            dst[y * dstW + x] = v;
        }
    }
    sceKernelDcacheWritebackRange(dst, dstW * dstH * 4);
    sceGuStart(GU_DIRECT, guList);
    sceGuDrawBuffer(GU_PSM_8888, drawBuf, BUF_W);  // (sceGuStart sends no address for buffer 0)
}

// Writes `bytes` of `buf` to `path` on the memory stick (debugging: a copy buffer, a texture).
void pg_save_raw(const char* path, const void* buf, unsigned int bytes)
{
    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0644);
    if (fd < 0) { port_trace("[ge] cannot write %s\n", path); return; }
    sceIoWrite(fd, buf, bytes);
    sceIoClose(fd);
    port_trace("[ge] wrote %u bytes to %s\n", bytes, path);
}

}  // extern "C"
