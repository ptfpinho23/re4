// port/src/port_gu: the PSP GE behind plain-typed wrappers (port_gu.h). Only this unit includes
// the pspsdk graphics headers.
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <string.h>
#include "port_gu.h"

#define BUF_W 512
#define FRAME_BYTES (BUF_W * PG_SCREEN_H * 4)

static unsigned int __attribute__((aligned(16))) guList[512 * 1024 / 4];

extern "C" {

void pg_init(void)
{
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

void pg_start(void) { sceGuStart(GU_DIRECT, guList); }

void pg_finish(void)
{
    sceGuFinish();
    sceGuSync(0, 0);
}

static int swaps;
void pg_swap(void)
{
    sceGuSwapBuffers();
    swaps++;
}

void pg_clear(unsigned int color, unsigned int depth, int colorToo, int depthToo)
{
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

void pg_texture(int psm, int w, int h, const void* data, int wrapS, int wrapT, int minFilt, int magFilt, int tfx)
{
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(psm, 0, 0, 0);
    sceGuTexImage(0, w, h, w, data);
    sceGuTexFilter(minFilt, magFilt);
    sceGuTexWrap(wrapS, wrapT);
    sceGuTexFunc(tfx, GU_TCC_RGBA);
    sceGuTexScale(1.0f, 1.0f);
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

void pg_draw(int prim, int count, const PgVertex* verts)
{
    sceGuDrawArray(prim, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, count, 0, verts);
}

void* pg_get_memory(int bytes) { return sceGuGetMemory(bytes); }

static int overlayInit;
void pg_overlay_begin(void)
{
    // buffer 0 is drawn first, so after an odd number of swaps buffer 0 is the one displayed
    void* shown = (void*) (0x44000000 + ((swaps & 1) ? 0 : FRAME_BYTES));
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

void pg_copy_frame(unsigned int* dst, int dstW, int dstH, int srcX, int srcY, int srcW, int srcH, int mode)
{
    // the list so far must have drawn before the CPU reads the buffer; then the list restarts
    sceGuFinish();
    sceGuSync(0, 0);
    unsigned int drawOfs = (swaps & 1) ? FRAME_BYTES : 0;
    const unsigned int* color = (const unsigned int*) (0x44000000 + drawOfs);
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
