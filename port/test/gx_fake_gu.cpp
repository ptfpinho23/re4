// Host test build: the GE wrapper (port_gu.h) replaced by a recorder. Draws and texture uploads
// are printed for tools/port/gxtest.py to check against its own reference.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port_gu.h"

static const void* lastTexData;
static int lastTexPsm, lastTexW, lastTexH, lastTexSet;

extern "C" {
void pg_init(void) {}
void pg_start(void) {}
void pg_finish(void) {}
void pg_swap(void) {}
void pg_clear(unsigned int color, unsigned int depth, int c, int d) { printf("CLEAR %08x %u\n", color, depth); }
void pg_viewport(float cx, float cy, float w, float h) { printf("VIEWPORT %g %g %g %g\n", cx, cy, w, h); }
void pg_scissor(int x, int y, int w, int h) { printf("SCISSOR %d %d %d %d\n", x, y, w, h); }
void pg_depth(int t, int f, int w) { printf("DEPTH %d %d %d\n", t, f, w); }
void pg_blend(int e, int op, int s, int d, unsigned int fs, unsigned int fd) { printf("BLEND %d %d %d %d %06x %06x\n", e, op, s, d, fs, fd); }
void pg_alpha_test(int e, int f, int r) { printf("ALPHA %d %d %d\n", e, f, r); }
void pg_cull(int e, int cw) { printf("CULL %d %d\n", e, cw); }
void pg_pixel_mask(unsigned int m) { printf("MASK %08x\n", m); }
void pg_fog(int e, float n, float f, unsigned int c) { printf("FOG %d %g %g %06x\n", e, n, f, c); }
void pg_texture_off(void) { lastTexSet = 0; }
void pg_texture(int psm, int w, int h, const void* data, int ws, int wt, int mn, int mg, int tfx)
{
    lastTexData = data; lastTexPsm = psm; lastTexW = w; lastTexH = h; lastTexSet = 1;
    printf("TEXSTATE %d %d %d %d %d\n", ws, wt, mn, mg, tfx);
}
void pg_projection(const float* m)
{
    printf("PROJ");
    for (int i = 0; i < 16; i++) printf(" %.9g", m[i]);
    printf("\n");
}
void pg_draw(int prim, int count, const PgVertex* v)
{
    printf("DRAW %d %d\n", prim, count);
    for (int i = 0; i < count; i++) printf("V %.9g %.9g %08x %.9g %.9g %.9g\n", v[i].u, v[i].v, v[i].color, v[i].x, v[i].y, v[i].z);
    if (lastTexSet) {
        int bytes = lastTexPsm == PG_PSM_DXT1 ? ((lastTexW + 3) / 4) * ((lastTexH + 3) / 4) * 8 : lastTexW * lastTexH * 4;
        printf("TEX %d %d %d ", lastTexPsm, lastTexW, lastTexH);
        const unsigned char* p = (const unsigned char*) lastTexData;
        for (int i = 0; i < bytes; i++) printf("%02x", p[i]);
        printf("\n");
    } else {
        printf("NOTEX\n");
    }
}
void* pg_get_memory(int bytes) { return malloc(bytes); }
void pg_overlay_begin(void) {}
void pg_debug_print(int c, int r, unsigned int col, const char* t) {}
void pg_dcache_writeback(const void* p, int n) {}
void* pg_uncached(void* p) { return p; }
void pg_wait_vblank(void) {}
}
