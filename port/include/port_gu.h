#ifndef PORT_GU_H
#define PORT_GU_H

// The PSP GE, through plain-typed wrappers (port/src/port_gu.cpp includes the pspsdk's pspgu.h,
// whose types clash with the game's; port/src/port_gx.cpp includes the game's and calls these).
#ifdef __cplusplus
extern "C" {
#endif

#define PG_SCREEN_W 480
#define PG_SCREEN_H 272

// vertex layout of every draw: texture 2 floats, colour 8888, position 3 floats (GU_TRANSFORM_3D)
struct PgVertex {
    float u, v;
    unsigned int color;
    float x, y, z;
};

// primitives (sceGu numbering)
#define PG_POINTS 0
#define PG_LINES 1
#define PG_LINE_STRIP 2
#define PG_TRIANGLES 3
#define PG_TRIANGLE_STRIP 4
#define PG_TRIANGLE_FAN 5

// pixel formats (sceGu numbering)
#define PG_PSM_5650 0
#define PG_PSM_5551 1
#define PG_PSM_4444 2
#define PG_PSM_8888 3
#define PG_PSM_DXT1 8

// texture functions (sceGu numbering)
#define PG_TFX_MODULATE 0
#define PG_TFX_DECAL 1
#define PG_TFX_BLEND 2
#define PG_TFX_REPLACE 3
#define PG_TFX_ADD 4

// depth / alpha test functions (sceGu numbering)
#define PG_NEVER 0
#define PG_ALWAYS 1
#define PG_EQUAL 2
#define PG_NOTEQUAL 3
#define PG_LESS 4
#define PG_LEQUAL 5
#define PG_GREATER 6
#define PG_GEQUAL 7

// blend operations / factors (sceGu numbering)
#define PG_ADD 0
#define PG_SUBTRACT 1
#define PG_REVERSE_SUBTRACT 2
#define PG_SRC_COLOR 0
#define PG_ONE_MINUS_SRC_COLOR 1
#define PG_SRC_ALPHA 2
#define PG_ONE_MINUS_SRC_ALPHA 3
#define PG_DST_COLOR 0
#define PG_ONE_MINUS_DST_COLOR 1
#define PG_DST_ALPHA 4
#define PG_ONE_MINUS_DST_ALPHA 5
#define PG_FIX 10

void pg_init(void);
void pg_start(void);
void pg_finish(void);
void pg_swap(void);
void pg_clear(unsigned int color, unsigned int depth, int colorToo, int depthToo);
void pg_viewport(float cx, float cy, float w, float h);
void pg_scissor(int x, int y, int w, int h);
void pg_depth(int testEnable, int func, int writeEnable);
void pg_blend(int enable, int op, int src, int dst, unsigned int fixSrc, unsigned int fixDst);
void pg_alpha_test(int enable, int func, int ref);
void pg_cull(int enable, int frontCW);
void pg_pixel_mask(unsigned int mask);
void pg_fog(int enable, float nearz, float farz, unsigned int color);
void pg_texture_off(void);
void pg_texture(int psm, int w, int h, const void* data, int wrapS, int wrapT, int minFilt, int magFilt, int tfx, int tcc);  // tcc 1: the texture alpha counts
void pg_projection(const float* m16);  // 16 floats, row-major (GameCube Mtx44)
void pg_draw(int prim, int count, const PgVertex* verts);
void* pg_get_memory(int bytes);
void pg_overlay_begin(void);  // targets the frame now on screen
void pg_debug_print(int col, int row, unsigned int color, const char* text);
void pg_dcache_writeback(const void* p, int bytes);
void* pg_uncached(void* p);
void pg_wait_vblank(void);
// Reads the frame being drawn (after finishing the GE's work) into an 8888 texture buffer, resampled
// from the source rectangle to dstW x dstH: mode 0 colour, 1 alpha as grey, 2 depth as grey.
void pg_copy_frame(unsigned int* dst, int dstW, int dstH, int srcX, int srcY, int srcW, int srcH, int mode);
// Writes the frame on screen as raw 480x272 ABGR8888 to a file (a screenshot without a host).
int pg_save_frame(const char* path);
void pg_request_dump(const char* path);  // saves the next finished frame (the list's own draw buffer)
int pg_dump_pending(void);               // 1 between pg_request_dump and the dump (port_gx traces the draws)

#ifdef __cplusplus
}
#endif
#endif
