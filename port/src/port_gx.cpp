// port/src/port_gx: the GameCube GX graphics API on the PSP GE.
//
// Geometry: GX vertices arrive either through the write-gather pipe (gx.h's inline writers, direct
// data in the current vertex format) or as display lists from the disc (GX command streams with
// direct or indexed attributes, big-endian). Both go through one parser here that resolves the
// attributes against the current vertex descriptor / format / arrays, transforms positions with
// the current position matrix on the CPU (matrix indices per vertex included), and hands the GE
// float vertices under the projection matrix. Mesh data therefore needs no endian conversion.
// Textures: converted from the GameCube tiled formats on first use (CMPR to the GE's DXT1, the
// rest to 32-bit) and cached by address until GXInvalidateTexAll. TEV: stage 0's texture and the
// vertex / material colour through the GE's texture function; multi-stage effects, indirect
// textures, hardware lighting and framebuffer copies are not reproduced yet.
#include "types.h"
#include "port.h"
#include "port_psp.h"
#include "port_gu.h"
extern "C" void port_trace(const char* fmt, ...);  // port_log.cpp (the log, not the screen)
#include "gx.h"
#include <string.h>
#include <stdint.h>
#ifdef __PSP__
#include <malloc.h>
#define ALIGNED_ALLOC(n) memalign(16, n)
#else
#define ALIGNED_ALLOC(n) aligned_alloc(16, ((n) + 15) & ~(size_t) 15)  // the host test build
#endif
#include <stdlib.h>
#include <math.h>

// ---------------------------------------------------------------- state
enum { A_PNMTX = 0, A_TEXMTX0 = 1, A_POS = 9, A_NRM = 10, A_CLR0 = 11, A_CLR1 = 12, A_TEX0 = 13, A_COUNT = 21 };

struct Fmt {
    u8 cnt, type, frac;
};
struct Vat {
    Fmt pos, nrm, clr[2], tex[8];
};

static u8 vcd[A_COUNT];          // GX_NONE / DIRECT / INDEX8 / INDEX16 per attribute
static Vat vat[8];
static const u8* arrayBase[A_COUNT];
static u32 arrayStride[A_COUNT];

static f32 mtxMem[64][4];        // GX matrix memory: rows of the 3x4 position / texture matrices
static f32 nrmMem[64][3];        // normal matrix memory: rows of the 3x3 normal matrices, same ids
static u32 currentMtx;

// Lighting on the CPU (the GE's lights work in its own model space; the game's are in view space
// and its vertices carry per-vertex matrix indices, so the lit colour is computed here).
struct Chan {
    u8 enable, ambSrc, matSrc, diffFn, attnFn;
    u32 lightMask;
};
static Chan chan[4];             // GX_COLOR0, GX_COLOR1, GX_ALPHA0, GX_ALPHA1
static u32 chanAmbColor[2] = {0, 0};
struct LightPort {               // kept inside the game's GXLightObj (0x40 bytes)
    u32 color;                   // ABGR
    f32 a0, a1, a2, k0, k1, k2;
    f32 px, py, pz;
    f32 dx, dy, dz;
};
static LightPort lights[8];
static f32 projection[4][4];
static int projType;
static f32 viewport[6];          // left, top, wd, ht, nearz, farz
static f32 efbW = 640.0f, efbH = 448.0f;

struct TexObjPort {              // overlays the 32-byte GXTexObj
    const void* data;
    u16 width, height;
    u8 format, wrapS, wrapT, mipmap;
    u32 tlutName;
    u8 minFilt, magFilt, isCI, pad;
    u32 spare[2];
};
struct TlutPort {                // overlays the 12-byte GXTlutObj
    const void* lut;
    u16 fmt, entries;
    u32 pad;
};
static TexObjPort* texMap[8];
static TlutPort tlutTable[20];    // GXLoadTlut copies the object: the game may build it on the stack
static u8 tlutLoaded[20];

struct TevStage {
    u8 coord, map, color, op;
    u8 cin[4], ain[4];           // GXSetTevColorIn / AlphaIn: GX_CC_* / GX_CA_* of a, b, c, d
    u8 kcsel;                    // GXSetTevKColorSel
};
static TevStage tev[16];
static u8 numTevStages = 1, numTexGens = 1, numChans = 1;
// GXSetTexCoordGen: how texture coordinate `dst` is generated: func 0 = 3x4 matrix (s, t, q; the
// coordinates are divided by q), 1 = 2x4; src 0 = the vertex position, 1 = the normal, 4.. = TEX0..;
// mtx the texture matrix (60 = identity; a vertex-supplied TEXMTX index overrides it), postMtx the
// post-transform matrix (125 = identity).
struct TexGen { u8 func, src, normalize; u32 mtx, postMtx; };
static TexGen texGen[8];
static f32 ptMem[64][4];         // post-transform texture matrices (ids 64..127), rows
static u32 chanMatColor[2] = {0xFFFFFFFF, 0xFFFFFFFF};
static u32 tevRegColor[4];
static u32 tevKColor[4];
// The draw's texture state, decided from the TEV stages (applyDrawState): the stage that samples a
// texture, its texture function, whether the texture alpha counts, and a constant colour that
// stands in for the rasterised colour when the stage multiplies the texture by a register.
static int drawConstColorOn;
static u32 drawConstColor;

static int cullMode;
static int blendType, blendSrc, blendDst;  // GXSetBlendMode, for the frame trace
static u32 copyClearColor = 0xFF000000;
static u32 copyClearZ = 0xFFFFFF;
static u16 copySrc[4];           // GXSetTexCopySrc: left, top, width, height (EFB pixels)
static u16 copyDstW, copyDstH;
static u8 copyDstFmt;
// Framebuffer copies (GXCopyTex): the game's destination buffer stays untouched and is the key; the
// frame is read back into an 8888 buffer of the copy's size that the texture cache hands out.
struct CopyRecord {
    const void* dest;
    u16 w, h;      // the copy the game asked for
    u16 pw, ph;    // the buffer's size: a power of two up to 512 the frame is resampled to
    u32* buf;
};
#define COPY_MAX 16
static CopyRecord copies[COPY_MAX];
static u32 colorMaskBits = 0;    // pixel mask bits (1 = write disabled)
static u32 alphaMaskBits = 0;
static int inited;

// ---------------------------------------------------------------- overlay log
#define OVERLAY_LINES 14
#define OVERLAY_COLS 60
static char overlay[OVERLAY_LINES][OVERLAY_COLS];
static u32 overlayStamp[OVERLAY_LINES];  // the frame the line arrived in
static int overlayNext;
static int overlayCount;
static u32 overlayFrame;
#define OVERLAY_KEEP_FRAMES (60 * 10)    // a line stays on screen for ten seconds

extern "C" void port_gx_overlay_line(const char* text)
{
    strncpy(overlay[overlayNext], text, OVERLAY_COLS - 1);
    overlay[overlayNext][OVERLAY_COLS - 1] = 0;
    char* nl = strchr(overlay[overlayNext], '\n');
    if (nl) *nl = 0;
    overlayStamp[overlayNext] = overlayFrame;
    overlayNext = (overlayNext + 1) % OVERLAY_LINES;
    if (overlayCount < OVERLAY_LINES) overlayCount++;
}

extern "C" int port_gx_active(void) { return inited; }

// Drawn straight into the displayed frame after the swap (the debug screen's pixel writer).
extern "C" int port_overlay_on;
static void drawOverlay(void)
{
    if (!port_overlay_on) return;
    overlayFrame++;
    int first = (overlayNext - overlayCount + OVERLAY_LINES) % OVERLAY_LINES;
    int shown = 0;
    for (int i = 0; i < overlayCount; i++) {
        int k = (first + i) % OVERLAY_LINES;
        if (overlayFrame - overlayStamp[k] > OVERLAY_KEEP_FRAMES) continue;  // old lines fade out
        if (!shown) pg_overlay_begin();
        pg_debug_print(0, shown++, 0xFF40FF40, overlay[k]);
    }
}

// ---------------------------------------------------------------- helpers
static inline u16 be16(const u8* p) { return (u16) ((p[0] << 8) | p[1]); }
static inline u32 be32(const u8* p) { return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3]; }
static inline f32 bef32(const u8* p)
{
    u32 v = be32(p);
    f32 f;
    memcpy(&f, &v, 4);
    return f;
}

static f32 sx(void) { return (f32) PG_SCREEN_W / efbW; }
static f32 sy(void) { return (f32) PG_SCREEN_H / efbH; }

static void applyViewport(void)
{
    f32 cx = 2048.0f - PG_SCREEN_W / 2.0f + (viewport[0] + viewport[2] * 0.5f) * sx();
    f32 cy = 2048.0f - PG_SCREEN_H / 2.0f + (viewport[1] + viewport[3] * 0.5f) * sy();
    pg_viewport(cx, cy, viewport[2] * sx(), viewport[3] * sy());
}

static void applyProjection(void)
{
    // GX clip z runs from -w (near) to 0 (far); the GE wants -w..w: row 2 <- 2 * row 2 + row 3.
    f32 m[16];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            f32 v = projection[r][c];
            if (r == 2) v = 2.0f * projection[2][c] + projection[3][c];
            m[r * 4 + c] = v;
        }
    }
    pg_projection(m);
}

static int compareFunc(int gx)
{
    static const int tbl[8] = {PG_NEVER, PG_LESS, PG_EQUAL, PG_LEQUAL, PG_GREATER, PG_NOTEQUAL, PG_GEQUAL, PG_ALWAYS};
    return tbl[gx & 7];
}

// ---------------------------------------------------------------- textures
struct TexCacheEntry {
    const void* data;
    const void* tlut;
    u16 w, h;          // the GameCube texture's size (the cache key)
    u16 pw, ph;        // the GE texture's size: a power of two up to 512 (fitTexture)
    f32 su, sv;        // UV scale from the GameCube size to the GE one
    u8 fmt;
    u8 psm;
    void* converted;
    u32* clut;         // the 8888 palette of a T4 / T8 conversion (16 or 256 entries), else NULL
    u16 clutN;
    u32 lastUse;
    u32 sig;           // sample hash of the source texels and palette (texSignature)
    u32 gen;           // the invalidation generation the signature was checked against
};
static u32 texGeneration = 1;  // bumped by GXInvalidateTexAll: entries re-check their signature before use
#define TEXCACHE_MAX 384
static TexCacheEntry texCache[TEXCACHE_MAX];
static u32 texUseClock;
static u32 texCacheBytes;      // converted bytes held; the PSP's heap is small, so the cache keeps a budget
extern "C" { unsigned int port_gx_stat_conversions, port_gx_stat_copies, port_gx_stat_dropped; }  // dropped: bad-vertex draws
extern "C" { unsigned int port_gx_stat_offscreen, port_gx_stat_onscreen; }
extern "C" { unsigned int port_gx_us_xform, port_gx_us_state, port_gx_us_gather, port_gx_us_native, port_gx_stat_native_draws, port_gx_stat_cpu_draws; }
extern "C" {
// The native path (drawNative): vertices of disc display lists handed to the GE in the model's
// space, for the GE to transform, light and texture-map. 0 keeps everything on the CPU path (the
// host test, and a debugger poke for comparisons).
#ifdef __PSP__
volatile int port_gx_native = 1;
#else
volatile int port_gx_native = 0;
#endif
unsigned int port_gx_stat_native, port_gx_stat_cpu;  // vertices per stats period by path
unsigned int port_gx_stat_why[8];                    // vertices kept on the CPU path, by reason
}
static u32 lightGen;                       // bumped when a light / channel set-up changes
static f32 boundSu = 1.0f, boundSv = 1.0f; // the bound texture's padding scale (applyDrawState)
extern "C" { volatile unsigned int port_gx_flags; }
static void* lastListCaller;  // who called GXCallDisplayList (the frame dump names it)
static u32 immQuadIndex;      // immediate-mode quads drawn this frame (the debugger's bisection)
static u32 lastListBytes;  // debugger pokes: 1 = no textures, 2 = no lighting (white), 4 = vertex colours only  // microseconds per stats period: vertex parsing/transform, draw state (texture cache)
#ifdef __PSP__
extern "C" unsigned int sceKernelGetSystemTimeLow(void);  // (the SDK headers clash with the game's)
extern "C" volatile int port_profile;
static inline u32 usNow(void) { return port_profile ? sceKernelGetSystemTimeLow() : 0; }
#else
static inline u32 usNow(void) { return 0; }
#endif  // per frame, for port_gu's [ge] frame line
#define TEXCACHE_BUDGET (5u << 20)
extern "C" unsigned int port_gx_texcache_bytes(void) { return texCacheBytes; }
// Bytes per texel of a GE pixel format, times 8 (T4 is half a byte).
static u32 psmBits(u8 psm)
{
    switch (psm) {
    case PG_PSM_T4: return 4;
    case PG_PSM_T8: return 8;
    case PG_PSM_5650: case PG_PSM_5551: case PG_PSM_4444: return 16;
    default: return 32;
    }
}
static u32 entryBytes(const TexCacheEntry* e)
{
    if (e->psm == PG_PSM_DXT1) return (u32) (e->pw / 4) * (e->ph / 4) * 8;
    if (e->psm == PG_PSM_DXT3) return (u32) (e->pw / 4) * (e->ph / 4) * 16;
    return ((u32) e->pw * e->ph * psmBits(e->psm)) / 8 + (e->clut ? e->clutN * 4u : 0u);
}
// Buffers an evicted texture used are freed only once the frame's GE work is done (GXCopyDisp):
// the display list may still reference them.
#define DEFERRED_MAX 512
static void* deferredFree[DEFERRED_MAX];
static int nDeferredFree;
static void deferFree(void* p)
{
    if (!p) return;
    if (nDeferredFree < DEFERRED_MAX) deferredFree[nDeferredFree++] = p;
    else free(p);  // (the list is full: the GE has most likely finished with an old one)
}
static void flushDeferredFree(void)
{
    for (int i = 0; i < nDeferredFree; i++) free(deferredFree[i]);
    nDeferredFree = 0;
}
static void freeEntry(TexCacheEntry* e)
{
    pg_texture_forget();
    texCacheBytes -= entryBytes(e);
    deferFree(e->converted);
    e->converted = NULL;
    if (e->clut) { deferFree(e->clut); e->clut = NULL; }
    e->clutN = 0;
}

static u32 pow2ceil(u32 v) { u32 p = 1; while (p < v) p <<= 1; return p; }

// A DXT1 block row decoder for the oversize CMPR case (fitTexture): ABGR8888 out.
static void decodeDxt1Block(const u8* b, u32* out, int stride, int bw, int bh)
{
    u16 c0 = (u16) (b[4] | (b[5] << 8)), c1 = (u16) (b[6] | (b[7] << 8));
    u32 pal[4];
    u32 r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31, r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
    r0 = r0 << 3 | r0 >> 2; g0 = g0 << 2 | g0 >> 4; b0 = b0 << 3 | b0 >> 2;
    r1 = r1 << 3 | r1 >> 2; g1 = g1 << 2 | g1 >> 4; b1 = b1 << 3 | b1 >> 2;
    pal[0] = 0xFF000000u | (b0 << 16) | (g0 << 8) | r0;
    pal[1] = 0xFF000000u | (b1 << 16) | (g1 << 8) | r1;
    if (c0 > c1) {
        pal[2] = 0xFF000000u | (((2 * b0 + b1) / 3) << 16) | (((2 * g0 + g1) / 3) << 8) | ((2 * r0 + r1) / 3);
        pal[3] = 0xFF000000u | (((b0 + 2 * b1) / 3) << 16) | (((g0 + 2 * g1) / 3) << 8) | ((r0 + 2 * r1) / 3);
    } else {
        pal[2] = 0xFF000000u | (((b0 + b1) / 2) << 16) | (((g0 + g1) / 2) << 8) | ((r0 + r1) / 2);
        pal[3] = 0;
    }
    for (int y = 0; y < bh; y++) {
        u8 row = b[y];  // GE order: texel 0 in the low bits
        for (int x = 0; x < bw; x++) out[y * stride + x] = pal[(row >> (x * 2)) & 3];
    }
}

// Expands a converted texture to 8888 (the palette or the 16-bit fields), for the downscale.
static u32* expandTo8888(const void* conv, u8 psm, const u32* clut, int w, int h)
{
    u32* out = (u32*) ALIGNED_ALLOC((u32) w * h * 4);
    if (!out) return NULL;
    const u8* p = (const u8*) conv;
    for (int i = 0; i < w * h; i++) {
        u32 c;
        switch (psm) {
        case PG_PSM_T4: { u32 idx = (p[i >> 1] >> ((i & 1) * 4)) & 15; c = clut ? clut[idx] : 0; break; }
        case PG_PSM_T8: c = clut ? clut[p[i]] : 0; break;
        case PG_PSM_5650: { u32 v = p[i * 2] | (p[i * 2 + 1] << 8), r = v & 31, g = (v >> 5) & 63, b = v >> 11;
            c = 0xFF000000u | ((b << 3 | b >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (r << 3 | r >> 2); break; }
        case PG_PSM_5551: { u32 v = p[i * 2] | (p[i * 2 + 1] << 8), r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31, a = v >> 15;
            c = ((a ? 255u : 0u) << 24) | ((b << 3 | b >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (r << 3 | r >> 2); break; }
        case PG_PSM_4444: { u32 v = p[i * 2] | (p[i * 2 + 1] << 8), r = v & 15, g = (v >> 4) & 15, b = (v >> 8) & 15, a = v >> 12;
            c = ((a * 17) << 24) | ((b * 17) << 16) | ((g * 17) << 8) | (r * 17); break; }
        default: c = ((const u32*) p)[i]; break;
        }
        out[i] = c;
    }
    return out;
}

// The GE takes power-of-two textures up to 512 x 512. Pads the converted image to the next power
// of two (edge texels replicated, so clamping samples the image's edge) and box-filters oversize
// ones down by a power of two (as 8888: the palette goes); CMPR is padded as blocks, or decoded
// first when it is oversize. Returns the buffer to bind (conv itself when it already fits), its
// size and the UV scale.
static void* fitTexture(void* conv, u8* psm, u32** clut, int w, int h, u16* pw, u16* ph, f32* su, f32* sv)
{
    u32 tw = pow2ceil((u32) w), th = pow2ceil((u32) h);
    if (tw < 8) tw = 8;
    if (th < 8) th = 8;
    int fx = 1, fy = 1;
    while (tw / fx > 512) fx <<= 1;
    while (th / fy > 512) fy <<= 1;
    *su = (f32) w / (f32) tw;
    *sv = (f32) h / (f32) th;
    if ((u32) w == tw && (u32) h == th && fx == 1 && fy == 1) { *pw = (u16) w; *ph = (u16) h; return conv; }
    if ((*psm == PG_PSM_DXT1 || *psm == PG_PSM_DXT3) && fx == 1 && fy == 1) {
        int bw = (w + 3) / 4, bh = (h + 3) / 4, pbw = tw / 4, pbh = th / 4, bb = *psm == PG_PSM_DXT3 ? 16 : 8;
        u8* out = (u8*) ALIGNED_ALLOC(pbw * pbh * bb);
        if (!out) return conv;
        for (int by = 0; by < pbh; by++) {
            int sy = by < bh ? by : bh - 1;
            for (int bx = 0; bx < pbw; bx++) {
                int sx = bx < bw ? bx : bw - 1;
                memcpy(out + (by * pbw + bx) * bb, (const u8*) conv + (sy * bw + sx) * bb, bb);
            }
        }
        free(conv);
        *pw = (u16) tw; *ph = (u16) th;
        return out;
    }
    if (fx == 1 && fy == 1 && *psm != PG_PSM_DXT1 && *psm != PG_PSM_DXT3) {  // padding only, in the format's own texel size
        u32 bits = psmBits(*psm);
        u32 rowBytes = (tw * bits) / 8, srcRow = ((u32) w * bits) / 8;
        u8* out = (u8*) ALIGNED_ALLOC(rowBytes * th);
        if (!out) return conv;
        const u8* src = (const u8*) conv;
        for (u32 y = 0; y < th; y++) {
            const u8* s = src + ((y < (u32) h ? y : (u32) h - 1) * srcRow);
            u8* d = out + y * rowBytes;
            memcpy(d, s, srcRow);
            if (bits >= 8) {  // replicate the last texel across the padding
                u32 tb = bits / 8;
                for (u32 x = (u32) w; x < tw; x++) memcpy(d + x * tb, s + ((u32) w - 1) * tb, tb);
            } else {          // T4: nibbles
                u32 last = (s[((u32) w - 1) >> 1] >> ((((u32) w - 1) & 1) * 4)) & 15;
                for (u32 x = (u32) w; x < tw; x++) {
                    u8* b = d + (x >> 1);
                    if (x & 1) *b = (u8) ((*b & 0x0F) | (last << 4)); else *b = (u8) ((*b & 0xF0) | last);
                }
            }
        }
        free(conv);
        *pw = (u16) tw; *ph = (u16) th;
        return out;
    }
    if (*psm != PG_PSM_DXT1 && *psm != PG_PSM_DXT3 && *psm != PG_PSM_8888) {
        // oversize in a compact format: keep it compact, halved by dropping texels (the fonts and
        // UI sheets this concerns are read one cell at a time; a box filter would cost 32 bits)
        u32 bits = psmBits(*psm);
        u32 ow = tw / fx, oh = th / fy;
        u32 rowBytes = (ow * bits) / 8;
        u8* out = (u8*) ALIGNED_ALLOC(rowBytes * oh);
        if (!out) return conv;
        const u8* sp = (const u8*) conv;
        u32 srcRow = ((u32) w * bits) / 8;
        for (u32 y = 0; y < oh; y++) {
            u32 sy = y * fy; if (sy >= (u32) h) sy = (u32) h - 1;
            u8* d = out + y * rowBytes;
            const u8* sr = sp + sy * srcRow;
            for (u32 x = 0; x < ow; x++) {
                u32 sx = x * fx; if (sx >= (u32) w) sx = (u32) w - 1;
                switch (bits) {
                case 4: { u32 v = (sr[sx >> 1] >> ((sx & 1) * 4)) & 15; u8* b = d + (x >> 1); if (x & 1) *b = (u8) ((*b & 0x0F) | (v << 4)); else *b = (u8) ((*b & 0xF0) | v); break; }
                case 8: d[x] = sr[sx]; break;
                default: ((u16*) d)[x] = ((const u16*) sr)[sx]; break;
                }
            }
        }
        free(conv);
        *pw = (u16) ow; *ph = (u16) oh;
        return out;
    }
    // oversize 8888 / CMPR: to 8888 first, then a box filter into the padded power-of-two size
    u32* src;
    if (*psm == PG_PSM_DXT1 || *psm == PG_PSM_DXT3) {
        int bw = (w + 3) / 4, bh = (h + 3) / 4, bb = *psm == PG_PSM_DXT3 ? 16 : 8;
        u32* dec = (u32*) ALIGNED_ALLOC(bw * 4 * bh * 4 * 4);
        if (!dec) { free(conv); return NULL; }  // (an oversize buffer must not reach the GE)
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                const u8* blk = (const u8*) conv + (by * bw + bx) * bb;
                u32* o = dec + (by * 4) * (bw * 4) + bx * 4;
                decodeDxt1Block(blk, o, bw * 4, 4, 4);
                if (bb == 16) {  // the alpha nibbles
                    for (int y = 0; y < 4; y++) {
                        u16 row = (u16) (blk[8 + y * 2] | (blk[9 + y * 2] << 8));
                        for (int x = 0; x < 4; x++) {
                            u32 a = (row >> (x * 4)) & 15;
                            o[y * bw * 4 + x] = (o[y * bw * 4 + x] & 0x00FFFFFFu) | ((a * 17) << 24);
                        }
                    }
                }
            }
        free(conv);
        src = dec;
        w = bw * 4; h = bh * 4;  // the decoded image is block-aligned; the scale stays the caller's
    } else if (*psm != PG_PSM_8888) {
        src = expandTo8888(conv, *psm, *clut, w, h);
        free(conv);
        if (*clut) { free(*clut); *clut = NULL; }
        if (!src) return NULL;
    } else {
        src = (u32*) conv;
    }
    *psm = PG_PSM_8888;
    int ow = (int) (tw / fx), oh = (int) (th / fy);
    u32* out = (u32*) ALIGNED_ALLOC(ow * oh * 4);
    if (!out) { if (src != conv) free(src); else free(conv); return NULL; }  // (an oversize buffer must not reach the GE)
    for (int y = 0; y < oh; y++) {
        int sy0 = y * fy;
        if (sy0 >= h) sy0 = h - 1;
        for (int x = 0; x < ow; x++) {
            int sx0 = x * fx;
            if (sx0 >= w) sx0 = w - 1;
            u32 r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int dy = 0; dy < fy; dy++) {
                int sy = sy0 + dy;
                if (sy >= h) sy = h - 1;
                for (int dx = 0; dx < fx; dx++) {
                    int sx = sx0 + dx;
                    if (sx >= w) sx = w - 1;
                    u32 c = src[sy * w + sx];
                    r += c & 0xFF; g += (c >> 8) & 0xFF; b += (c >> 16) & 0xFF; a += c >> 24; n++;
                }
            }
            out[y * ow + x] = ((a / n) << 24) | ((b / n) << 16) | ((g / n) << 8) | (r / n);
        }
    }
    free(src);
    *pw = (u16) ow; *ph = (u16) oh;
    return out;
}

static u32 tlutColor(const u8* lut, int fmt, int index)
{
    u16 v = be16(lut + index * 2);
    switch (fmt) {
    case 0: {  // IA8
        u8 a = v >> 8, i = v & 0xFF;
        return ((u32) a << 24) | ((u32) i << 16) | ((u32) i << 8) | i;
    }
    case 1: {  // RGB565
        u8 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        return 0xFF000000u | ((u32) (b << 3 | b >> 2) << 16) | ((u32) (g << 2 | g >> 4) << 8) | (r << 3 | r >> 2);
    }
    default: {  // RGB5A3
        if (v & 0x8000) {
            u8 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
            return 0xFF000000u | ((u32) (b << 3 | b >> 2) << 16) | ((u32) (g << 3 | g >> 2) << 8) | (r << 3 | r >> 2);
        }
        u8 a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
        return ((u32) (a << 5 | a << 2 | a >> 1) << 24) | ((u32) (b * 17) << 16) | ((u32) (g * 17) << 8) | (r * 17);
    }
    }
}

// Decodes a GameCube texture for the GE: CMPR is re-blocked as DXT1, the intensity and palette
// formats become 4 / 8-bit indexed textures with an 8888 CLUT (exact, and a quarter of the memory
// of the 32-bit form the PSP cannot afford), RGB565 becomes 5650, RGB5A3 5551 (opaque) or 4444,
// and IA8 / RGBA8 / C14X2 stay 32-bit ABGR.
static void* convertTexture(const TexObjPort* t, const TlutPort* tlut, u8* psmOut, u32** clutOut, u16* clutN)
{
    int w = t->width, h = t->height, fmt = t->format;
    const u8* src = (const u8*) t->data;
    *clutOut = NULL;
    *clutN = 0;
    if (fmt == 14) {  // CMPR: 8x8 tiles of four 4x4 DXT1 blocks -> GE DXT1 (indices then colours)
        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        int tilesX = (w + 7) / 8, tilesY = (h + 7) / 8;
        // A block whose first colour is not above the second is in the three-colour mode: its
        // index 3 is transparent. The GE's DXT1 has no such mode (the texel comes out black), so
        // such a texture becomes DXT3: the same colours, plus a 4-bit alpha per texel.
        int punch = 0;
        for (int i = 0; i < tilesX * tilesY * 4 && !punch; i++) {
            u16 c0 = (u16) ((src[i * 8] << 8) | src[i * 8 + 1]), c1 = (u16) ((src[i * 8 + 2] << 8) | src[i * 8 + 3]);
            if (c0 <= c1) punch = 1;
        }
        int bb = punch ? 16 : 8;
        *psmOut = punch ? PG_PSM_DXT3 : PG_PSM_DXT1;
        u8* out = (u8*) ALIGNED_ALLOC(bw * bh * bb);
        if (!out) return NULL;
        for (int ty = 0; ty < tilesY; ty++) {
            for (int tx = 0; tx < tilesX; tx++) {
                for (int sub = 0; sub < 4; sub++) {
                    int bx = tx * 2 + (sub & 1), by = ty * 2 + (sub >> 1);
                    if (bx >= bw || by >= bh) { src += 8; continue; }
                    u8* d = out + (by * bw + bx) * bb;
                    u16 c0 = (u16) ((src[0] << 8) | src[1]), c1 = (u16) ((src[2] << 8) | src[3]);
                    int three = c0 <= c1;
                    int swap = punch && three && c0 != c1;  // keep the GE in the four-colour mode
                    for (int i = 0; i < 4; i++) {  // index rows: GC keeps texel 0 in the top bits
                        u8 b = src[4 + i];
                        u8 r = (u8) (((b & 0x03) << 6) | ((b & 0x0C) << 2) | ((b & 0x30) >> 2) | ((b & 0xC0) >> 6));
                        if (punch) {
                            u16 alpha = 0;
                            u8 r2 = 0;
                            for (int x = 0; x < 4; x++) {
                                u32 idx = (r >> (x * 2)) & 3;
                                u32 a = 15;
                                if (three && idx == 3) { a = 0; idx = 0; }
                                else if (swap && idx < 2) idx ^= 1;
                                r2 |= (u8) (idx << (x * 2));
                                alpha |= (u16) (a << (x * 4));
                            }
                            r = r2;
                            d[8 + i * 2] = (u8) alpha; d[9 + i * 2] = (u8) (alpha >> 8);
                        }
                        d[i] = r;
                    }
                    if (swap) { d[4] = src[3]; d[5] = src[2]; d[6] = src[1]; d[7] = src[0]; }
                    else { d[4] = src[1]; d[5] = src[0]; d[6] = src[3]; d[7] = src[2]; }  // colours, little-endian
                    src += 8;
                }
            }
        }
        if (punch) {
            static u32 nTr;
            if (++nTr <= 12) port_trace("[gx] CMPR %ux%u uses the transparent mode: DXT3\n", (unsigned) w, (unsigned) h);
        }
        return out;
    }
    const u8* lut = tlut ? (const u8*) tlut->lut : NULL;
    int lutFmt = tlut ? tlut->fmt : 2;
    // the output kind and, for the indexed kinds, the palette
    u8 psm;
    u32* clut = NULL;
    int n = 0;
    switch (fmt) {
    case 0: psm = PG_PSM_T4; n = 16; break;                     // I4: grey ramp, A = I
    case 1: case 2: psm = PG_PSM_T8; n = 256; break;             // I8, IA4
    case 4: psm = PG_PSM_5650; break;                            // RGB565
    case 5: {                                                    // RGB5A3: 5551 when every texel is opaque, else 8888 (exact)
        psm = PG_PSM_5551;
        int tiles = ((w + 3) / 4) * ((h + 3) / 4);
        for (int i = 0; i < tiles * 16 && psm == PG_PSM_5551; i++) if (!(src[i * 2] & 0x80)) psm = PG_PSM_8888;
        break;
    }
    case 8: psm = PG_PSM_T4; n = 16; break;                      // C4
    case 9: psm = PG_PSM_T8; n = 256; break;                     // C8
    default: psm = PG_PSM_8888; break;                           // IA8, RGBA8, C14X2
    }
    if (n) {
        clut = (u32*) ALIGNED_ALLOC(n * 4);
        if (!clut) return NULL;
        for (int i = 0; i < n; i++) {
            u32 c;
            if (fmt == 0) { u32 v = (u32) i * 17; c = (v << 24) | (v << 16) | (v << 8) | v; }
            else if (fmt == 1) { u32 v = (u32) i; c = (v << 24) | (v << 16) | (v << 8) | v; }
            else if (fmt == 2) { u32 a = (u32) (i >> 4) * 17, l = (u32) (i & 15) * 17; c = (a << 24) | (l << 16) | (l << 8) | l; }
            else c = lut ? tlutColor(lut, lutFmt, i) : 0xFF000000u;
            clut[i] = c;
        }
    }
    u32 bits = psmBits(psm);
    u32 bytes = ((u32) w * h * bits + 7) / 8;
    u8* out = (u8*) ALIGNED_ALLOC(bytes);
    if (!out) { if (clut) free(clut); return NULL; }
    memset(out, 0, bytes);
    int tw = 4, th = 4;  // tile size
    switch (fmt) {
    case 0: case 8: tw = 8; th = 8; break;              // I4, C4
    case 1: case 2: case 9: tw = 8; th = 4; break;      // I8, IA4, C8
    default: break;                                     // IA8, RGB565, RGB5A3, RGBA8, C14X2
    }
    for (int ty = 0; ty < (h + th - 1) / th; ty++) {
        for (int tx = 0; tx < (w + tw - 1) / tw; tx++) {
            for (int y = 0; y < th; y++) {
                for (int x = 0; x < tw; x++) {
                    int px = tx * tw + x, py = ty * th + y;
                    int i = y * tw + x;
                    if (px >= w || py >= h) continue;
                    u32 o = (u32) py * w + px;
                    switch (fmt) {
                    case 0: case 8: {  // 4-bit index (I4 grey level or C4 palette index), texel 0 in the high nibble
                        u8 v = src[i >> 1]; v = (i & 1) ? (v & 15) : (v >> 4);
                        u8* b = out + (o >> 1);
                        if (o & 1) *b = (u8) ((*b & 0x0F) | (v << 4)); else *b = (u8) ((*b & 0xF0) | v);
                        break;
                    }
                    case 1: case 2: case 9: out[o] = src[i]; break;  // I8 level / IA4 byte / C8 index
                    case 3: { u8 a = src[i * 2], l = src[i * 2 + 1]; ((u32*) out)[o] = (u32) a << 24 | l << 16 | l << 8 | l; break; }
                    case 4: { u16 v = be16(src + i * 2); u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                        ((u16*) out)[o] = (u16) (r | (g << 5) | (b << 11)); break; }
                    case 5: {
                        u16 v = be16(src + i * 2);
                        if (psm == PG_PSM_5551) { u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31; ((u16*) out)[o] = (u16) (r | (g << 5) | (b << 10) | 0x8000); }
                        else ((u32*) out)[o] = tlutColor(src + i * 2, 2, 0);  // the 8888 decode of one RGB5A3 texel
                        break;
                    }
                    case 6: { u8 a = src[i * 2], r = src[i * 2 + 1], g = src[32 + i * 2], b = src[32 + i * 2 + 1]; ((u32*) out)[o] = (u32) a << 24 | b << 16 | g << 8 | r; break; }
                    case 10: ((u32*) out)[o] = lut ? tlutColor(lut, lutFmt, be16(src + i * 2) & 0x3FFF) : 0xFF000000u; break;
                    default: ((u32*) out)[o] = 0xFFFF00FF; break;
                    }
                }
            }
            src += (fmt == 0 || fmt == 8) ? 32 : (fmt == 1 || fmt == 2 || fmt == 9) ? 32 : (fmt == 6) ? 64 : 32;
        }
    }
    *psmOut = psm;
    *clutOut = clut;
    *clutN = (u16) n;
    return out;
}

static u32 texBytes(const TexObjPort* t)
{
    u32 w = t->width, h = t->height;
    switch (t->format) {
    case 0: case 8: return ((w + 7) / 8) * ((h + 7) / 8) * 32;             // I4, C4
    case 1: case 2: case 9: return ((w + 7) / 8) * ((h + 3) / 4) * 32;     // I8, IA4, C8
    case 6: return ((w + 3) / 4) * ((h + 3) / 4) * 64;                     // RGBA8
    case 14: return ((w + 7) / 8) * ((h + 7) / 8) * 32;                    // CMPR
    default: return ((w + 3) / 4) * ((h + 3) / 4) * 32;                    // IA8, RGB565, RGB5A3, C14X2
    }
}

// A cheap signature of a texture's contents: 64 samples spread over the texels plus the palette.
// The game invalidates the whole texture cache (GXInvalidateTexAll) far more often than it changes
// texture memory, so the cache keeps its conversions and re-checks this instead of reconverting.
static u32 texSignature(const TexObjPort* t, const TlutPort* tlut)
{
    u32 h = 2166136261u;
    const u8* d = (const u8*) t->data;
    u32 n = texBytes(t);
    if (d && n) {
        u32 step = n / 64;
        if (step < 4) step = 4;
        for (u32 i = 0; i < n; i += step) h = (h ^ *(const u32*) (d + (i & ~3u))) * 16777619u;
    }
    if (tlut && tlut->lut) {
        const u8* l = (const u8*) tlut->lut;
        u32 m = (u32) tlut->entries * 2;
        for (u32 i = 0; i < m; i += 4) h = (h ^ *(const u32*) (l + i)) * 16777619u;
    }
    return h;
}

// The entry a texture's data pointer last resolved to: a draw binds the texture of the draw before
// it far more often than not, and the cache is a few hundred entries to walk.
static struct { const void* data; TexCacheEntry* e; } texMemo[64];
static inline u32 memoSlot(const void* d) { return (((u32) (uintptr_t) d) >> 5) & 63; }

static TexCacheEntry* cachedTexture(const TexObjPort* t)
{
#ifdef __PSP__
    u32 addr = (u32) (uintptr_t) t->data & 0x1FFFFFFF;
    int badAddr = addr < 0x08400000 || addr >= 0x0C000000;
#else
    int badAddr = t->data == NULL;
#endif
    if (t->width == 0 || t->height == 0 || t->width > 1024 || t->height > 1024 || t->format > 14 || badAddr) {
        static u32 nb;  // a texture object that was never initialised, or file data the loader did not relocate
        if (++nb <= 20) port_trace("[gx] bad texture object: data %p %ux%u fmt %d\n", t->data, t->width, t->height, t->format);
        return NULL;
    }
    const TlutPort* tlut = (t->isCI && t->tlutName < 20 && tlutLoaded[t->tlutName]) ? &tlutTable[t->tlutName] : NULL;
    const void* lutPtr = tlut ? tlut->lut : NULL;
    {
        TexCacheEntry* e = texMemo[memoSlot(t->data)].e;
        if (e && texMemo[memoSlot(t->data)].data == t->data && e->converted && e->data == t->data && e->tlut == lutPtr
            && e->w == t->width && e->h == t->height && e->fmt == t->format && e->gen == texGeneration) {
            e->lastUse = ++texUseClock;
            return e;
        }
    }
    for (int i = 0; i < COPY_MAX; i++) {
        if (copies[i].buf && copies[i].dest == t->data) {
            static TexCacheEntry copyEntry;
            copyEntry.data = t->data; copyEntry.tlut = NULL;
            copyEntry.w = copyEntry.pw = copies[i].pw; copyEntry.h = copyEntry.ph = copies[i].ph;  // the frame was resampled to the GE size
            copyEntry.su = copyEntry.sv = 1.0f;
            copyEntry.fmt = t->format; copyEntry.psm = PG_PSM_8888;
            copyEntry.converted = copies[i].buf;
            copyEntry.clut = NULL; copyEntry.clutN = 0;
            return &copyEntry;
        }
    }
    TexCacheEntry* victim = NULL;
    for (int i = 0; i < TEXCACHE_MAX; i++) {
        TexCacheEntry* e = &texCache[i];
        if (e->converted && e->data == t->data && e->tlut == lutPtr && e->w == t->width && e->h == t->height && e->fmt == t->format) {
            if (e->gen != texGeneration) {  // invalidated since: still the same texels?
                u32 sig = texSignature(t, tlut);
                if (sig != e->sig) {
                    { static u32 nr; if (++nr <= 8 || (nr & 127) == 0) port_trace("[gx] miss %u: %p %ux%u fmt %d tlut %p changed (signature %08x -> %08x)\n", nr, t->data, t->width, t->height, t->format, lutPtr, e->sig, sig); }
                    freeEntry(e);
                    victim = e;
                    break;
                }
                e->gen = texGeneration;
            }
            e->lastUse = ++texUseClock;
            texMemo[memoSlot(t->data)].data = t->data; texMemo[memoSlot(t->data)].e = e;
            return e;
        }
        if (!e->converted) {
            if (!victim || victim->converted) victim = e;  // a free slot beats any eviction
        } else if (!victim || (victim->converted && e->lastUse < victim->lastUse)) {
            victim = e;                                   // else the least recently used
        }
    }
    if (!victim) victim = &texCache[0];
    {
        static u32 nm;
        int show = ++nm <= 8 || (nm & 127) == 0;
        if (show && !victim->converted) {
            int same = -1, used = 0;
            for (int i = 0; i < TEXCACHE_MAX; i++) {
                if (texCache[i].converted) used++;
                if (texCache[i].converted && texCache[i].data == t->data && same < 0) same = i;
            }
            if (same >= 0) {
                const TexCacheEntry* e = &texCache[same];
                port_trace("[gx] miss %u: %p %ux%u fmt %d tlut %p: an entry with that data exists (tlut %p %ux%u fmt %d), %d used\n", nm, t->data, t->width, t->height, t->format, lutPtr, e->tlut, e->w, e->h, e->fmt, used);
            } else {
                port_trace("[gx] miss %u: %p %ux%u fmt %d tlut %p: new (%d used)\n", nm, t->data, t->width, t->height, t->format, lutPtr, used);
            }
        } else if (show && victim->converted) {
            port_trace("[gx] miss %u: %p %ux%u fmt %d: evicting %p %ux%u (cache %u KB)\n", nm, t->data, t->width, t->height, t->format, victim->data, victim->w, victim->h, texCacheBytes / 1024);
        }
    }
    if (victim->converted) freeEntry(victim);
    // the budget: evict the least recently used conversions until the new one fits (the size the
    // conversion will have: indexed, 16-bit, DXT1 or 32-bit by format)
    u32 needBits = (t->format == 0 || t->format == 8) ? 4 : (t->format == 1 || t->format == 2 || t->format == 9) ? 8
                 : (t->format == 4 || t->format == 5) ? 16 : t->format == 14 ? 4 : 32;
    for (u32 need = ((u32) t->width * t->height * needBits) / 8 + 1024; texCacheBytes + need > TEXCACHE_BUDGET;) {
        TexCacheEntry* old = NULL;
        for (int i = 0; i < TEXCACHE_MAX; i++) {
            TexCacheEntry* e = &texCache[i];
            if (e->converted && (!old || e->lastUse < old->lastUse)) old = e;
        }
        if (!old) break;
        freeEntry(old);
    }
    u8 psm = PG_PSM_8888;
    u32* clut = NULL;
    u16 clutN = 0;
    void* conv = convertTexture(t, tlut, &psm, &clut, &clutN);
    if (!conv) {
        static int nf;
        if (++nf <= 10) port_trace("[gx] texture %ux%u fmt %d: no memory for the conversion\n", t->width, t->height, t->format);
        return NULL;
    }
    port_gx_stat_conversions++;
    {
        static u32 nConv;
        nConv++;
        if (nConv <= 64 || (nConv & 63) == 0) {  // the first ones, then every 64th: a cache that thrashes shows here
            port_trace("[gx] texture %u: %p %ux%u fmt %d tlut %p -> psm %d, %u KB cached\n", nConv, t->data, t->width, t->height, t->format, lutPtr, psm, texCacheBytes / 1024);
        }
    }
    u16 pw, ph;
    f32 su, sv;
    conv = fitTexture(conv, &psm, &clut, t->width, t->height, &pw, &ph, &su, &sv);
    if (!conv) { if (clut) free(clut); return NULL; }
    if (psm == PG_PSM_DXT1 || psm == PG_PSM_DXT3) clut = NULL;
    pg_dcache_writeback(conv, psm == PG_PSM_DXT1 ? (pw / 4) * (ph / 4) * 8 : psm == PG_PSM_DXT3 ? (pw / 4) * (ph / 4) * 16 : (pw * ph * psmBits(psm)) / 8);
    if (clut) pg_dcache_writeback(clut, clutN * 4);
    victim->pw = pw; victim->ph = ph; victim->su = su; victim->sv = sv;
    victim->clut = clut; victim->clutN = clut ? clutN : 0;
    victim->data = t->data;
    victim->tlut = lutPtr;
    victim->w = t->width;
    victim->h = t->height;
    victim->fmt = t->format;
    victim->psm = psm;
    victim->converted = conv;
    texCacheBytes += entryBytes(victim);
    victim->lastUse = ++texUseClock;
    victim->sig = texSignature(t, tlut);
    victim->gen = texGeneration;
    texMemo[memoSlot(t->data)].data = t->data; texMemo[memoSlot(t->data)].e = victim;
    return victim;
}

static void invalidateTextures(void)
{
    texGeneration++;  // the entries re-check their source signature when next bound
}

static void freeTextures(void)
{
    for (int i = 0; i < TEXCACHE_MAX; i++) {
        if (texCache[i].converted) freeEntry(&texCache[i]);
    }
}

// ---------------------------------------------------------------- per-draw state
static inline int inputsMention(const u8* in, u8 v) { return in[0] == v || in[1] == v || in[2] == v || in[3] == v; }

// The stage the GE's single texture unit stands in for: the first active stage whose colour
// combiner reads the texture colour (GX_CC_TEXC 8, or TEXA / TEXRRR.. 9, 16..18) through a bound map.
static int textureStage(void)
{
    if (numTexGens == 0) return -1;
    int n = numTevStages < 1 ? 1 : numTevStages > 16 ? 16 : numTevStages;
    for (int i = 0; i < n; i++) {
        const TevStage* st = &tev[i];
        if (st->map == 0xFF || st->map >= 8 || texMap[st->map] == NULL) continue;
        int usesTex = 0;
        for (int k = 0; k < 4; k++) {
            if (st->cin[k] == 8 || st->cin[k] == 9 || st->cin[k] >= 16) usesTex = 1;
            if (st->ain[k] == 4) usesTex = 1;  // GX_CA_TEXA
        }
        if (usesTex) return i;
    }
    return -1;
}

static int texturedDraw(void) { return textureStage() >= 0; }

// A colour register / constant the combiner multiplies the texture by (GX_CC_C0..C2 2/4/6, KONST 14).
static int constInput(u8 v, u32* color)
{
    if (v == 2 || v == 4 || v == 6) { *color = tevRegColor[v / 2]; return 1; }
    if (v == 14) return 0;  // KONST: resolved per stage by the caller
    return 0;
}

static void applyDrawStateImpl(int hasVertexColor);
static void applyDrawState(int hasVertexColor)
{
    u32 t0 = usNow();
    applyDrawStateImpl(hasVertexColor);
    if (port_gx_flags & 1) pg_texture_off();
    port_gx_us_state += usNow() - t0;
}
static void applyDrawStateImpl(int hasVertexColor)
{
    drawConstColorOn = 0;
    boundSu = boundSv = 1.0f;
    int si = textureStage();
    if (si < 0) {
        pg_texture_off();
        return;
    }
    const TevStage* st = &tev[si];
    TexObjPort* t = texMap[st->map];
    TexCacheEntry* e = cachedTexture(t);
    if (!e) {
        pg_texture_off();
        return;
    }
    // colour: d + (1 - c) * a + c * b with a = ZERO (15): c * b -> texture times c, or d alone
    int tfx = PG_TFX_MODULATE;
    const u8* c = st->cin;
    u8 other = 0xFF;
    if (c[0] == 15 && c[3] == 15) {
        if (c[1] == 8) other = c[2]; else if (c[2] == 8) other = c[1];
    }
    if (c[0] == 15 && c[1] == 15 && c[2] == 15 && c[3] == 8) {
        tfx = PG_TFX_REPLACE;                    // texture only
    } else if (other == 12) {
        tfx = PG_TFX_REPLACE;                    // texture * ONE
    } else if (other == 10 || other == 0xFF) {
        tfx = PG_TFX_MODULATE;                   // texture * rasterised colour (or unknown: modulate)
    } else {
        u32 col = 0xFFFFFFFF;
        int isConst = constInput(other, &col);
        if (other == 14) {                       // KONST: the stage's constant selection
            int sel = st->kcsel;
            if (sel >= 0x0C && sel <= 0x0F) { col = tevKColor[sel - 0x0C]; isConst = 1; }
            else if (sel <= 7) { u32 v = (u32) ((8 - sel) * 255 / 8); col = 0xFF000000u | (v << 16) | (v << 8) | v; isConst = 1; }
        }
        if (isConst) {                           // texture * register: the register replaces the vertex colour
            drawConstColorOn = 1;
            drawConstColor = col;
        }
        tfx = PG_TFX_MODULATE;
    }
    if (st->op == 1) tfx = PG_TFX_DECAL;         // GX_DECAL through GXSetTevOp
    int tcc = inputsMention(st->ain, 4) ? 1 : 0;  // the texture alpha counts only when the alpha combiner reads it
    boundSu = e->su; boundSv = e->sv;
    pg_texture(e->psm, e->pw, e->ph, e->converted, t->wrapS == 0 ? 1 : 0, t->wrapT == 0 ? 1 : 0,
               t->minFilt == 0 ? 0 : 1, t->magFilt == 0 ? 0 : 1, tfx, tcc, e->su, e->sv, e->clut, e->clutN);
}

// ---------------------------------------------------------------- vertex parsing
struct Parser {
    const u8* p;
    int bigEndian;  // disc display lists; the write-gather data is native
};

static inline u32 readIndex(Parser* s, u8 type)
{
    u32 i;
    if (type == 2) {  // INDEX8
        i = *s->p;
        s->p += 1;
    } else {          // INDEX16
        i = s->bigEndian ? be16(s->p) : (u32) (s->p[0] | (s->p[1] << 8));
        s->p += 2;
    }
    return i;
}

static inline f32 readComp(const u8* p, u8 type, int bigEndian, u8 frac)
{
    f32 v;
    switch (type) {
    case 0: v = (f32) p[0]; break;                                                  // u8
    case 1: v = (f32) (s8) p[0]; break;                                             // s8
    case 2: v = (f32) (bigEndian ? be16(p) : (u16) (p[0] | (p[1] << 8))); break;    // u16
    case 3: v = (f32) (s16) (bigEndian ? be16(p) : (u16) (p[0] | (p[1] << 8))); break;  // s16
    default: {
        if (bigEndian) v = bef32(p); else memcpy(&v, p, 4);
        return v;
    }
    }
    return frac ? v / (f32) (1 << frac) : v;
}

static inline int compSize(u8 type) { return type == 0 || type == 1 ? 1 : type == 4 ? 4 : 2; }

static u32 readColor(const u8* p, u8 type, int bigEndian, int* size)
{
    u8 r = 255, g = 255, b = 255, a = 255;
    switch (type) {
    case 0: { u16 v = bigEndian ? be16(p) : (u16) (p[0] | (p[1] << 8)); r = (u8) ((v >> 11) << 3); g = (u8) (((v >> 5) & 63) << 2); b = (u8) ((v & 31) << 3); *size = 2; break; }
    case 1: r = p[0]; g = p[1]; b = p[2]; *size = 3; break;
    case 2: r = p[0]; g = p[1]; b = p[2]; *size = 4; break;
    case 3: { u16 v = bigEndian ? be16(p) : (u16) (p[0] | (p[1] << 8)); r = (u8) (((v >> 12) & 15) * 17); g = (u8) (((v >> 8) & 15) * 17); b = (u8) (((v >> 4) & 15) * 17); a = (u8) ((v & 15) * 17); *size = 2; break; }
    case 4: { u32 v = ((u32) p[0] << 16) | ((u32) p[1] << 8) | p[2]; r = (u8) (((v >> 18) & 63) << 2); g = (u8) (((v >> 12) & 63) << 2); b = (u8) (((v >> 6) & 63) << 2); a = (u8) ((v & 63) << 2); *size = 3; break; }
    default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; *size = 4; break;
    }
    return ((u32) a << 24) | ((u32) b << 16) | ((u32) g << 8) | r;
}

// The colour channel pair TEV stage 0 rasterises: 0 = COLOR0A0, 1 = COLOR1A1, -1 = none / zero.
static int rasChannel(void)
{
    u8 c = tev[0].color;
    if (c == 4) return 0;   // GX_COLOR0A0
    if (c == 5) return 1;   // GX_COLOR1A1
    return -1;
}

static u32 defaultColor(void)
{
    int ch = rasChannel();
    if (ch < 0) return 0xFFFFFFFF;
    // GX_SRC_REG: the material register colour; otherwise (or when no vertex colour) white.
    return chan[ch].matSrc == 0 ? chanMatColor[ch] : 0xFFFFFFFF;
}

static inline f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// GX vertex lighting of channel pair `ch`: illum = ambient + sum of light colour * attenuation *
// diffuse, clamped, times the material; the alpha channel is not lit by the game (enable 0).
static u32 lightColor(int ch, u32 vertexColor, const f32* n, f32 x, f32 y, f32 z)
{
    const Chan* c = &chan[ch];
    u32 mat = c->matSrc == 0 ? chanMatColor[ch] : vertexColor;
    u32 amb = c->ambSrc == 0 ? chanAmbColor[ch] : vertexColor;
    f32 ir = (f32) (amb & 0xFF) / 255.0f, ig = (f32) ((amb >> 8) & 0xFF) / 255.0f, ib = (f32) ((amb >> 16) & 0xFF) / 255.0f;
    for (int i = 0; i < 8; i++) {
        if (!(c->lightMask & (1u << i))) continue;
        const LightPort* l = &lights[i];
        f32 lx = l->px - x, ly = l->py - y, lz = l->pz - z;
        f32 d2 = lx * lx + ly * ly + lz * lz;
        f32 d = sqrtf(d2);
        if (d > 0.0f) { lx /= d; ly /= d; lz /= d; }
        f32 diff = 1.0f;
        if (c->diffFn != 0) {
            diff = n[0] * lx + n[1] * ly + n[2] * lz;
            if (c->diffFn == 2 && diff < 0.0f) diff = 0.0f;  // GX_DF_CLAMP
        }
        f32 att = 1.0f;
        if (c->attnFn == 1) {  // GX_AF_SPOT
            f32 cosA = -(lx * l->dx + ly * l->dy + lz * l->dz);
            f32 aatt = l->a0 + l->a1 * cosA + l->a2 * cosA * cosA;
            if (aatt < 0.0f) aatt = 0.0f;
            f32 den = l->k0 + l->k1 * d + l->k2 * d2;
            att = den > 0.0f ? aatt / den : 0.0f;
        }
        f32 k = att * diff;
        if (k <= 0.0f) continue;
        ir += k * (f32) (l->color & 0xFF) / 255.0f;
        ig += k * (f32) ((l->color >> 8) & 0xFF) / 255.0f;
        ib += k * (f32) ((l->color >> 16) & 0xFF) / 255.0f;
    }
    u32 r = (u32) ((f32) (mat & 0xFF) * clamp01(ir) + 0.5f);
    u32 g = (u32) ((f32) ((mat >> 8) & 0xFF) * clamp01(ig) + 0.5f);
    u32 b = (u32) ((f32) ((mat >> 16) & 0xFF) * clamp01(ib) + 0.5f);
    const Chan* ca = &chan[ch + 2];
    u32 a = (ca->matSrc == 0 ? chanMatColor[ch] : vertexColor) >> 24;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

static void transformPos(const f32* m, f32 x, f32 y, f32 z, PgVertex* v)
{
    v->x = m[0] * x + m[1] * y + m[2] * z + m[3];
    v->y = m[4] * x + m[5] * y + m[6] * z + m[7];
    v->z = m[8] * x + m[9] * y + m[10] * z + m[11];
}

// Texture coordinate generation (the XF unit): the source vector (position, normal or TEX0)
// through the texture matrix (3x4: s, t, q; 2x4: s, t) and the post matrix, then divided by q.
// The division is per vertex here (per pixel on the GameCube).
static void genTexCoord(const TexGen* g, u32 mtxId, const f32* pos, const f32* nrm, f32 u, f32 v, f32* s, f32* t)
{
    f32 in[4];
    if (g->src == 0) { in[0] = pos[0]; in[1] = pos[1]; in[2] = pos[2]; in[3] = 1.0f; }
    else if (g->src == 1) { in[0] = nrm[0]; in[1] = nrm[1]; in[2] = nrm[2]; in[3] = 1.0f; }
    else { in[0] = u; in[1] = v; in[2] = 1.0f; in[3] = 1.0f; }
    f32 o[3];
    if (mtxId == 60 || mtxId >= 64) {
        o[0] = in[0]; o[1] = in[1]; o[2] = g->func == 0 ? in[2] : 1.0f;
    } else {
        const f32* m = &mtxMem[mtxId][0];
        o[0] = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + m[3] * in[3];
        o[1] = m[4] * in[0] + m[5] * in[1] + m[6] * in[2] + m[7] * in[3];
        o[2] = g->func == 0 ? m[8] * in[0] + m[9] * in[1] + m[10] * in[2] + m[11] * in[3] : 1.0f;
    }
    if (g->normalize) {
        f32 len = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (len > 0.0f) { o[0] /= len; o[1] /= len; o[2] /= len; }
    }
    if (g->postMtx != 125 && g->postMtx >= 64 && g->postMtx + 2 < 128) {
        const f32* p = &ptMem[g->postMtx - 64][0];
        f32 r0 = p[0] * o[0] + p[1] * o[1] + p[2] * o[2] + p[3];
        f32 r1 = p[4] * o[0] + p[5] * o[1] + p[6] * o[2] + p[7];
        f32 r2 = p[8] * o[0] + p[9] * o[1] + p[10] * o[2] + p[11];
        o[0] = r0; o[1] = r1; o[2] = r2;
    }
    f32 q = o[2];
    if (g->func == 0 && q != 0.0f && q != 1.0f) { o[0] /= q; o[1] /= q; }
    *s = o[0];
    *t = o[1];
}

// Parses `count` vertices of vertex format `vf` from the stream and draws them as `prim`.
static const u8* drawVerticesImpl(Parser* s, int prim, int vf, u32 count);

// The projection's near plane as a view-space z (GX perspective: m23 / (m22 - 1) is the near distance).
static f32 nearPlane(void)
{
    f32 m22 = projection[2][2], m23 = projection[2][3];
    f32 n = (m22 - 1.0f) != 0.0f ? m23 / (m22 - 1.0f) : 1.0f;
    if (!(n > 0.0f) || n > 1.0e6f) n = 1.0f;
    return -n;
}

static inline void lerpVertex(const PgVertex* a, const PgVertex* b, f32 t, PgVertex* o)
{
    o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t; o->z = a->z + (b->z - a->z) * t;
    o->u = a->u + (b->u - a->u) * t; o->v = a->v + (b->v - a->v) * t;
    u32 c = 0;
    for (int k = 0; k < 4; k++) {
        int ca = (a->color >> (k * 8)) & 0xFF, cb = (b->color >> (k * 8)) & 0xFF;
        c |= (u32) (ca + (int) ((cb - ca) * t)) << (k * 8);
    }
    o->color = c;
}

// Clips one triangle against the plane z = zn (keeps z <= zn): 0, 3 or 6 vertices out.
static u32 clipTriangleNear(const PgVertex* a, const PgVertex* b, const PgVertex* c, f32 zn, PgVertex* out)
{
    const PgVertex* in[3] = {a, b, c};
    PgVertex poly[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const PgVertex* p = in[i];
        const PgVertex* q = in[(i + 1) % 3];
        int pin = p->z <= zn, qin = q->z <= zn;
        if (pin) poly[n++] = *p;
        if (pin != qin) {
            f32 t = (zn - p->z) / (q->z - p->z);
            lerpVertex(p, q, t, &poly[n++]);
        }
    }
    if (n < 3) return 0;
    out[0] = poly[0]; out[1] = poly[1]; out[2] = poly[2];
    if (n == 3) return 3;
    out[3] = poly[0]; out[4] = poly[2]; out[5] = poly[3];
    return 6;
}
static const u8* drawVertices(Parser* s, int prim, int vf, u32 count)
{
    u32 t0 = usNow();
    const u8* r = drawVerticesImpl(s, prim, vf, count);
    port_gx_us_xform += usNow() - t0;
    return r;
}

// ---------------------------------------------------------------- the native path
static inline const u8* attrPtr(const u8* sp, u8 mode, const u8* base, u32 stride)
{
    if (mode == 1) return sp;                                    // direct: the data is in the stream
    if (mode == 2) return base + (u32) sp[0] * stride;           // INDEX8
    return base + (u32) ((sp[0] << 8) | sp[1]) * stride;         // INDEX16 (big-endian)
}
static inline u32 attrStreamSize(u8 mode, u32 direct) { return mode == 1 ? direct : mode == 2 ? 1 : mode == 3 ? 2 : 0; }

// Draws `count` vertices of a disc display list through the GE's own transform: the attributes
// are gathered (de-indexed, byte-swapped) into a GE vertex in the model's space, the model matrix
// is the game's current position matrix (with the fixed-point scale of 16-bit positions folded
// in), the lights are the GE's and the texture coordinates go through the GE's scale or texture
// matrix. Returns the stream position after the vertices, or NULL when the draw needs something
// the GE cannot do the game's way (the CPU path then takes it): per-vertex matrix indices,
// generated texture coordinates from positions / normals, more than four lights, spot angle
// attenuation, an ambient colour from the vertices.
static const u8* drawNative(Parser* s, int prim, int vf, u32 count)
{
    const Vat* f = &vat[vf];
#define WHY(n) do { port_gx_stat_why[n] += count; return NULL; } while (0)
    if (prim != 0 && prim != 2 && prim != 3 && prim != 4) WHY(0);  // quads, triangles, strips, fans
    if (vcd[A_PNMTX]) WHY(1);
    for (int k = 0; k < 8; k++) if (vcd[A_TEXMTX0 + k]) WHY(1);
    const u8 posMode = vcd[A_POS], nrmMode = vcd[A_NRM], clrMode = vcd[A_CLR0], texMode = vcd[A_TEX0];
    if (!posMode || f->pos.cnt != 1 || (f->pos.type != 1 && f->pos.type != 3 && f->pos.type != 4)) {
        static u32 nTr;
        if (++nTr <= 6) port_trace("[gx] native: position mode %d type %d cnt %d frac %d (colour mode %d type %d)\n", posMode, f->pos.type, f->pos.cnt, f->pos.frac, vcd[A_CLR0], f->clr[0].type);
        WHY(2);
    }
    if (posMode != 1 && !arrayBase[A_POS]) WHY(2);
    const int ch = rasChannel();
    const Chan* c = ch >= 0 ? &chan[ch] : NULL;
    const Chan* ca = ch >= 0 ? &chan[ch + 2] : NULL;
    const int lit = c && c->enable;
    if (lit && !nrmMode) WHY(3);
    const u8 nrmType = f->nrm.type;
    if (nrmMode) {
        if (f->nrm.cnt != 0 || (nrmType != 1 && nrmType != 3 && nrmType != 4)) WHY(3);
        if (nrmMode != 1 && !arrayBase[A_NRM]) WHY(3);
    }
    const u8 clrType = f->clr[0].type;
    if (clrMode && (clrType > 5 || (clrMode != 1 && !arrayBase[A_CLR0]))) {
        static u32 nTr;
        if (++nTr <= 6) port_trace("[gx] native: colour mode %d type %d base %p\n", clrMode, clrType, arrayBase[A_CLR0]);
        WHY(2);
    }
    const u8 texType = f->tex[0].type;
    if (texMode && (f->tex[0].cnt != 1 || texType > 4 || (texMode != 1 && !arrayBase[A_TEX0]))) WHY(4);
    const int si = textureStage();
    const int hasTex = texMode != 0 && si >= 0;
    const TexGen* g = &texGen[0];
    int texMatrix = 0;
    if (si >= 0) {
        if (!texMode || g->src != 4 || g->normalize || g->postMtx != 125) WHY(4);  // generated / post-transformed coordinates
        if (g->mtx != 60 && g->mtx < 64) {
            if (g->func == 0) WHY(4);                                                // a 3x4 matrix with the q divide
            texMatrix = 1;
        }
    }
    // the stream: POS, NRM, CLR0, CLR1, TEX0..7, each direct or an 8 / 16-bit index
    static const u8 clrSize[6] = {2, 3, 4, 2, 3, 4};
    u32 off = 0;
    const u32 posOff = off; off += attrStreamSize(posMode, compSize(f->pos.type) * 3);
    const u32 nrmOff = off; off += attrStreamSize(nrmMode, compSize(nrmType) * 3);
    const u32 clrOff = off; off += attrStreamSize(clrMode, clrSize[clrType % 6]);
    off += attrStreamSize(vcd[A_CLR1], clrSize[f->clr[1].type % 6]);
    const u32 texOff = off; off += attrStreamSize(texMode, compSize(texType) * 2);
    for (int k = 1; k < 8; k++) off += attrStreamSize(vcd[A_TEX0 + k], compSize(f->tex[k].type) * (f->tex[k].cnt ? 2 : 1));
    const u32 streamBytes = off;
    PgNative nat;
    nat.nLights = 0;
    f32 ambientExtra[3] = {0.0f, 0.0f, 0.0f};  // lights left out of the GE's four
    u8 lightIdx[8];
    if (lit) {
        if ((c->ambSrc != 0 && c->matSrc != 0) || c->attnFn == 0 || ca->enable) {
            static u32 nTr;
            if (++nTr <= 6) port_trace("[gx] native: lighting model ambSrc %d matSrc %d attnFn %d diffFn %d alpha enable %d mask %x\n", c->ambSrc, c->matSrc, c->attnFn, c->diffFn, ca->enable, (unsigned) c->lightMask);
            WHY(5);
        }
        for (int i = 0; i < 8; i++) {
            if (!(c->lightMask & (1u << i))) continue;
            const LightPort* l = &lights[i];
            f32 a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
            u32 lcol = l->color;
            f32 peak = 1.0f, cutoff = 0.0f, exponent = 0.0f;
            int spot = 0;
            if (c->attnFn == 1) {  // GX_AF_SPOT: angle attenuation over distance attenuation
                if (l->k0 == 0.0f && l->k1 == 0.0f && l->k2 == 0.0f) continue;   // (no light at any distance)
                peak = l->a0;
                if (l->a1 != 0.0f || l->a2 != 0.0f) {
                    // a0 + a1 cos: zero at the cutoff cosine -a0 / a1, rising to a0 + a1 on the axis
                    // (GXInitLightSpot's GX_SP_COS family). The GE's spot is cos^exponent inside
                    // the cutoff: exact for a 90 degree cone, matched at the cone's middle otherwise.
                    const f32 len2 = l->dx * l->dx + l->dy * l->dy + l->dz * l->dz;
                    cutoff = l->a1 > 0.0f ? -l->a0 / l->a1 : 2.0f;
                    peak = l->a0 + l->a1;
                    if (l->a2 != 0.0f || cutoff <= -0.99f || cutoff >= 0.99f || peak <= 0.0f || len2 < 0.98f || len2 > 1.02f) {
                        static u32 nTr;
                        if (++nTr <= 10) port_trace("[gx] native: light %d angle %g %g %g dist %g %g %g pos %g %g %g dir %g %g %g diffFn %d\n", i, l->a0, l->a1, l->a2, l->k0, l->k1, l->k2, l->px, l->py, l->pz, l->dx, l->dy, l->dz, c->diffFn);
                        WHY(6);
                    }
                    spot = 1;
                    const f32 mid = (1.0f + cutoff) * 0.5f;
                    exponent = cutoff == 0.0f ? 1.0f : logf(0.5f) / logf(mid);
                }
                if (peak <= 0.0f) continue;
                // the constant factor is a brightness: into the colour when it brightens (the GE
                // clamps the attenuation at 1), into the distance terms when it dims
                a0 = l->k0; a1 = l->k1; a2 = l->k2;
                if (peak > 1.0f) {
                    u32 r = (u32) ((f32) (lcol & 0xFF) * peak), gg = (u32) ((f32) ((lcol >> 8) & 0xFF) * peak), b = (u32) ((f32) ((lcol >> 16) & 0xFF) * peak);
                    lcol = (lcol & 0xFF000000u) | ((b > 255 ? 255 : b) << 16) | ((gg > 255 ? 255 : gg) << 8) | (r > 255 ? 255 : r);
                } else if (peak < 1.0f) {
                    a0 /= peak; a1 /= peak; a2 /= peak;
                }
            }
            if (nat.nLights == 8) WHY(7);
            lightIdx[nat.nLights] = (u8) i;
            PgNativeLight* o = &nat.light[nat.nLights++];
            o->ambientOnly = c->diffFn == 0;
            o->pos[0] = l->px; o->pos[1] = l->py; o->pos[2] = l->pz;
            o->color = lcol;
            o->att[0] = a0; o->att[1] = a1; o->att[2] = a2;
            o->spot = spot;
            o->dir[0] = l->dx; o->dir[1] = l->dy; o->dir[2] = l->dz;
            o->exponent = exponent; o->cutoff = cutoff;
        }
        if (nat.nLights > 4) {
            // the GE has four lights: the ones that add nothing visible to this draw are left out
            // (a room's lamps reach a few metres; a model far from them still lists them). Their
            // strength is taken at the draw's first, middle and last vertex.
            f32 sample[3][3];
            for (int k = 0; k < 3; k++) {
                const u32 n = k == 0 ? 0 : k == 1 ? count / 2 : count - 1;
                const u8* p = attrPtr(s->p + n * streamBytes + posOff, posMode, arrayBase[A_POS], arrayStride[A_POS]);
                f32 x, y, z;
                if (f->pos.type == 4) { x = bef32(p); y = bef32(p + 4); z = bef32(p + 8); }
                else {
                    const f32 q = 1.0f / (f32) (1u << (f->pos.frac & 31));
                    if (f->pos.type == 3) { x = (f32) (s16) be16(p) * q; y = (f32) (s16) be16(p + 2) * q; z = (f32) (s16) be16(p + 4) * q; }
                    else { x = (f32) (s8) p[0] * q; y = (f32) (s8) p[1] * q; z = (f32) (s8) p[2] * q; }
                }
                const f32* m = &mtxMem[currentMtx][0];
                sample[k][0] = m[0] * x + m[1] * y + m[2] * z + m[3];
                sample[k][1] = m[4] * x + m[5] * y + m[6] * z + m[7];
                sample[k][2] = m[8] * x + m[9] * y + m[10] * z + m[11];
            }
            f32 strength[8];
            for (int i = 0; i < nat.nLights; i++) {
                const PgNativeLight* o = &nat.light[i];
                const u32 col = o->color;
                u32 top = col & 0xFF;
                if (((col >> 8) & 0xFF) > top) top = (col >> 8) & 0xFF;
                if (((col >> 16) & 0xFF) > top) top = (col >> 16) & 0xFF;
                f32 best = 0.0f;
                for (int k = 0; k < 3; k++) {
                    const f32 dx = o->pos[0] - sample[k][0], dy = o->pos[1] - sample[k][1], dz = o->pos[2] - sample[k][2];
                    const f32 d2 = dx * dx + dy * dy + dz * dz;
                    const f32 den = o->att[0] + o->att[1] * sqrtf(d2) + o->att[2] * d2;
                    const f32 v = den > 1.0f ? (f32) top / den : (f32) top;
                    if (v > best) best = v;
                }
                strength[i] = best;
            }
            while (nat.nLights > 4) {
                int weakest = 0;
                for (int i = 1; i < nat.nLights; i++) if (strength[i] < strength[weakest]) weakest = i;
                if (strength[weakest] >= 1.5f) {  // (of 255)
                    // a faint light becomes ambient: a quarter of it, the mean of its diffuse term
                    // over all the directions a surface can face
                    if (strength[weakest] >= 12.0f || c->ambSrc != 0) {
                        static u32 nTr;
                        if (++nTr <= 6) port_trace("[gx] native: %d lights, the weakest adds %g\n", nat.nLights, strength[weakest]);
                        WHY(7);
                    }
                    const PgNativeLight* o = &nat.light[weakest];
                    const u32 col = o->color;
                    u32 top = col & 0xFF;
                    if (((col >> 8) & 0xFF) > top) top = (col >> 8) & 0xFF;
                    if (((col >> 16) & 0xFF) > top) top = (col >> 16) & 0xFF;
                    const f32 k = (o->ambientOnly ? 1.0f : 0.25f) * strength[weakest] / (f32) (top ? top : 1);
                    ambientExtra[0] += (f32) (col & 0xFF) * k;
                    ambientExtra[1] += (f32) ((col >> 8) & 0xFF) * k;
                    ambientExtra[2] += (f32) ((col >> 16) & 0xFF) * k;
                }
                for (int i = weakest; i + 1 < nat.nLights; i++) { nat.light[i] = nat.light[i + 1]; strength[i] = strength[i + 1]; lightIdx[i] = lightIdx[i + 1]; }
                nat.nLights--;
            }
        }
    }
#undef WHY
    const u8* sp = s->p;
    const u8* end = sp + count * streamBytes;
    if (cullMode == 3) return end;  // GX_CULL_ALL
    const f32* m = &mtxMem[currentMtx][0];
    for (int i = 0; i < 12; i++) {
        if (!(m[i] == m[i])) { port_gx_stat_dropped++; return end; }
    }
    const int hasColor = clrMode != 0;
    applyDrawState(hasColor);
    if (drawConstColorOn && lit) { port_gx_stat_why[5] += count; return NULL; }
    // the GE vertex: texture f32 x 2, colour, normal (when lit), position
    u32 o = 0, vtype = PG_VT_COLOR_8888;
    if (hasTex) { vtype |= PG_VT_TEX_F32; o = 8; }
    const u32 clrOut = o; o += 4;
    const u32 nrmOut = o;
    if (lit) {
        if (nrmType == 1) { o += 3; vtype |= PG_VT_NRM_S8; }
        else if (nrmType == 3) { o += 6; vtype |= PG_VT_NRM_S16; }
        else { o += 12; vtype |= PG_VT_NRM_F32; }
    }
    const int pos16 = f->pos.type == 3, pos8 = f->pos.type == 1;
    u32 posOut;
    if (pos8) { posOut = o; o += 3; vtype |= PG_VT_POS_S8; }
    else if (pos16) { o = (o + 1) & ~1u; posOut = o; o += 6; vtype |= PG_VT_POS_S16; }
    else { o = (o + 3) & ~3u; posOut = o; o += 12; vtype |= PG_VT_POS_F32; }
    const u32 stride = (o + 3) & ~3u;
    const int quads = prim == 0;
    const u32 outCount = quads ? count / 4 * 6 : count;
    u8* out = (u8*) pg_get_memory((int) (outCount * stride));
    if (!out) return end;
    // the colour a vertex carries: the material of a lit draw (the GE multiplies the light sum by
    // it and takes its alpha), the rasterised colour of an unlit one
    u32 baseColor, vmask;
    if (lit && c->ambSrc != 0) {
        // (vertex colour + lights) x register material: the vertex colour is the GE's ambient
        // material, the register the global ambient and the diffuse material
        const u32 reg = chanMatColor[ch];
        baseColor = 0xFFFFFFFFu;
        vmask = hasColor ? (0x00FFFFFFu | (ca->matSrc != 0 ? 0xFF000000u : 0)) : 0;
        nat.ambient = (reg & 0x00FFFFFFu) | (ca->matSrc == 0 ? (reg & 0xFF000000u) : 0xFF000000u);
        nat.colorMaterial = 1;
        nat.material = reg | 0xFF000000u;
    } else if (lit) {
        // (register ambient + lights) x material: the material (vertex or register colour, with the
        // alpha channel's own source) is composed into the vertex colour
        const u32 reg = chanMatColor[ch];
        baseColor = (c->matSrc == 0 ? (reg & 0x00FFFFFFu) : 0x00FFFFFFu) | (ca->matSrc == 0 ? (reg & 0xFF000000u) : 0xFF000000u);
        vmask = hasColor ? ((c->matSrc != 0 ? 0x00FFFFFFu : 0) | (ca->matSrc != 0 ? 0xFF000000u : 0)) : 0;
        u32 amb = chanAmbColor[ch];
        if (ambientExtra[0] + ambientExtra[1] + ambientExtra[2] > 0.0f) {
            u32 r = (amb & 0xFF) + (u32) (ambientExtra[0] + 0.5f), g2 = ((amb >> 8) & 0xFF) + (u32) (ambientExtra[1] + 0.5f), b = ((amb >> 16) & 0xFF) + (u32) (ambientExtra[2] + 0.5f);
            amb = (r > 255 ? 255 : r) | ((g2 > 255 ? 255 : g2) << 8) | ((b > 255 ? 255 : b) << 16);
        }
        nat.ambient = amb | 0xFF000000u;
        nat.colorMaterial = 3;
        nat.material = 0xFFFFFFFFu;
    } else {
        baseColor = defaultColor();
        vmask = hasColor ? 0xFFFFFFFFu : 0;
    }
    const u32 constMask = drawConstColorOn ? 0x00FFFFFFu : 0;
    const u32 constColor = drawConstColor & 0x00FFFFFFu;
    const f32 ku = texType == 4 ? 1.0f : 1.0f / (f32) (1u << (f->tex[0].frac & 31));
    const u8* posBase = arrayBase[A_POS]; const u32 posStride = arrayStride[A_POS];
    const u8* nrmBase = arrayBase[A_NRM]; const u32 nrmStride = arrayStride[A_NRM];
    const u8* clrBase = arrayBase[A_CLR0]; const u32 clrStride = arrayStride[A_CLR0];
    const u8* texBase = arrayBase[A_TEX0]; const u32 texStride = arrayStride[A_TEX0];
    u8 qbuf[4][40] __attribute__((aligned(4)));
    u8* d = out;
    const u32 tGather = usNow();
    for (u32 n = 0; n < count; n++, sp += streamBytes) {
        u8* v = quads ? qbuf[n & 3] : d;
        if (hasTex) {
            const u8* p = attrPtr(sp + texOff, texMode, texBase, texStride);
            f32 tu, tv;
            switch (texType) {
            case 3: tu = (f32) (s16) ((p[0] << 8) | p[1]) * ku; tv = (f32) (s16) ((p[2] << 8) | p[3]) * ku; break;
            case 4: tu = bef32(p); tv = bef32(p + 4); break;
            case 2: tu = (f32) ((p[0] << 8) | p[1]) * ku; tv = (f32) ((p[2] << 8) | p[3]) * ku; break;
            case 1: tu = (f32) (s8) p[0] * ku; tv = (f32) (s8) p[1] * ku; break;
            default: tu = (f32) p[0] * ku; tv = (f32) p[1] * ku; break;
            }
            ((f32*) v)[0] = tu;
            ((f32*) v)[1] = tv;
        }
        u32 col = baseColor;
        if (vmask) {
            const u8* p = attrPtr(sp + clrOff, clrMode, clrBase, clrStride);
            int sz;
            const u32 vc = clrType == 5 ? ((u32) p[0] | ((u32) p[1] << 8) | ((u32) p[2] << 16) | ((u32) p[3] << 24)) : readColor(p, clrType, 1, &sz);
            col = (vc & vmask) | (baseColor & ~vmask);
        }
        *(u32*) (v + clrOut) = (col & ~constMask) | (constColor & constMask);
        if (lit) {
            const u8* p = attrPtr(sp + nrmOff, nrmMode, nrmBase, nrmStride);
            if (nrmType == 1) {
                v[nrmOut] = p[0]; v[nrmOut + 1] = p[1]; v[nrmOut + 2] = p[2];
            } else if (nrmType == 3) {
                u16* q = (u16*) (v + nrmOut);
                q[0] = be16(p); q[1] = be16(p + 2); q[2] = be16(p + 4);
            } else {
                f32* q = (f32*) (v + nrmOut);
                q[0] = bef32(p); q[1] = bef32(p + 4); q[2] = bef32(p + 8);
            }
        }
        {
            const u8* p = attrPtr(sp + posOff, posMode, posBase, posStride);
            if (pos8) {
                v[posOut] = p[0]; v[posOut + 1] = p[1]; v[posOut + 2] = p[2];
            } else if (pos16) {
                u16* q = (u16*) (v + posOut);
                q[0] = be16(p); q[1] = be16(p + 2); q[2] = be16(p + 4);
            } else {
                f32* q = (f32*) (v + posOut);
                q[0] = bef32(p); q[1] = bef32(p + 4); q[2] = bef32(p + 8);
            }
        }
        if (!quads) {
            d += stride;
        } else if ((n & 3) == 3) {  // a quad: two triangles
            memcpy(d, qbuf[0], stride); memcpy(d + stride, qbuf[1], stride); memcpy(d + 2 * stride, qbuf[2], stride);
            memcpy(d + 3 * stride, qbuf[0], stride); memcpy(d + 4 * stride, qbuf[2], stride); memcpy(d + 5 * stride, qbuf[3], stride);
            d += 6 * stride;
        }
    }
    port_gx_us_gather += usNow() - tGather;
    port_gx_stat_native_draws++;
    // the GE's side of it
    nat.vtype = vtype;
    // the GE reads 16-bit positions as value / 32768 and 8-bit ones as value / 128; the game's are value / 2^frac
    const int unit = pos16 ? 15 : pos8 ? 7 : -1, pf = f->pos.frac & 31;
    const f32 k = unit < 0 ? 1.0f : pf <= unit ? (f32) (1u << (unit - pf)) : 1.0f / (f32) (1u << (pf - unit));
    for (int r = 0; r < 3; r++) {
        nat.model[r * 4 + 0] = m[r * 4 + 0] * k;
        nat.model[r * 4 + 1] = m[r * 4 + 1] * k;
        nat.model[r * 4 + 2] = m[r * 4 + 2] * k;
        nat.model[r * 4 + 3] = m[r * 4 + 3];
    }
    nat.texMatrix = texMatrix;
    nat.texScale[0] = boundSu; nat.texScale[1] = boundSv;
    if (texMatrix) {
        const f32* r0 = &mtxMem[g->mtx][0];
        const f32* r1 = &mtxMem[g->mtx + 1][0];
        nat.texMtx[0] = r0[0] * boundSu; nat.texMtx[1] = r0[1] * boundSu; nat.texMtx[2] = (r0[2] + r0[3]) * boundSu;
        nat.texMtx[3] = r1[0] * boundSv; nat.texMtx[4] = r1[1] * boundSv; nat.texMtx[5] = (r1[2] + r1[3]) * boundSv;
    }
    nat.lit = lit;
    u32 lightSel = 0;  // (which of the game's lights: a draw may leave faint ones out)
    for (int i = 0; i < nat.nLights; i++) lightSel |= 1u << lightIdx[i];
    nat.lightSig = (lightGen << 9) | (lightSel << 1) | (u32) (ch & 1);
    static const int primTbl[5] = {PG_TRIANGLES, PG_TRIANGLES, PG_TRIANGLES, PG_TRIANGLE_STRIP, PG_TRIANGLE_FAN};
    port_gx_stat_native += count;
    pg_draw_native(primTbl[prim], (int) outCount, out, (int) (outCount * stride), &nat);
    return end;
}

static const u8* drawVerticesImpl(Parser* s, int prim, int vf, u32 count)
{
    if (vf > 7 || count == 0) return s->p;
    if (port_gx_native && s->bigEndian && !port_gx_flags && !pg_dump_pending()) {
        const u32 tn = usNow();
        const u8* r = drawNative(s, prim, vf, count);
        port_gx_us_native += usNow() - tn;
        if (r) return r;
    }
    port_gx_stat_cpu += count;
    port_gx_stat_cpu_draws++;
    const Vat* f = &vat[vf];
    int quads = prim == 0;
    u32 outCount = quads ? count / 4 * 6 : count;
    PgVertex* out = (PgVertex*) pg_get_memory(outCount * sizeof(PgVertex));
    int discard = out == NULL;  // no room this frame: the stream is still parsed (the caller continues after it)
    int hasColor = vcd[A_CLR0] != 0;
    u32 defColor = defaultColor();
    int ch = rasChannel();
    int lit = ch >= 0 && chan[ch].enable && vcd[A_NRM] != 0;
    const f32* curMtx = &mtxMem[currentMtx][0];
    const f32* curNrm = &nrmMem[currentMtx][0];
    u32 o = 0;
    for (u32 n = 0; n < count; n++) {
        PgVertex v;
        const f32* m = curMtx;
        const f32* nm = curNrm;
        v.u = v.v = 0.0f;
        v.color = defColor;
        v.x = v.y = v.z = 0.0f;
        f32 px = 0, py = 0, pz = 0;
        f32 nx = 0, ny = 0, nz = 1;
        u32 texMtx0 = texGen[0].mtx;
        for (int a = 0; a < A_COUNT; a++) {
            u8 t = vcd[a];
            if (t == 0) continue;
            if (a == A_PNMTX) {
                u32 idx = *s->p++;
                if (idx < 64) { m = &mtxMem[idx][0]; nm = &nrmMem[idx][0]; }
                continue;
            }
            if (a >= A_TEXMTX0 && a < A_POS) {
                u32 idx = *s->p++;
                if (a == A_TEXMTX0 && idx < 64) texMtx0 = idx;  // per-vertex texture matrix index
                continue;
            }
            const u8* d;
            if (t == 1) {
                d = s->p;
            } else {
                u32 idx = readIndex(s, t);
                d = arrayBase[a] + idx * arrayStride[a];
                if (!arrayBase[a]) continue;
            }
            int size = 0;
            if (a == A_POS) {
                int cs = compSize(f->pos.type);
                px = readComp(d, f->pos.type, s->bigEndian, f->pos.frac);
                py = readComp(d + cs, f->pos.type, s->bigEndian, f->pos.frac);
                pz = f->pos.cnt ? readComp(d + 2 * cs, f->pos.type, s->bigEndian, f->pos.frac) : 0.0f;
                size = cs * (f->pos.cnt ? 3 : 2);
            } else if (a == A_NRM) {
                int cs = compSize(f->nrm.type);
                if (lit || texGen[0].src == 1) {  // s8 normals are 1.0 = 64, s16 1.0 = 16384 (GX fixes the fraction)
                    u8 frac = f->nrm.type == 1 ? 6 : f->nrm.type == 3 ? 14 : 0;
                    nx = readComp(d, f->nrm.type, s->bigEndian, frac);
                    ny = readComp(d + cs, f->nrm.type, s->bigEndian, frac);
                    nz = readComp(d + 2 * cs, f->nrm.type, s->bigEndian, frac);
                }
                size = cs * (f->nrm.cnt ? 9 : 3);
            } else if (a == A_CLR0 || a == A_CLR1) {
                const Fmt* c = &f->clr[a - A_CLR0];
                u32 col = readColor(d, c->type, s->bigEndian, &size);
                if (a == A_CLR0) v.color = col;
            } else {
                const Fmt* tf = &f->tex[a - A_TEX0];
                int cs = compSize(tf->type);
                if (a == A_TEX0) {
                    v.u = readComp(d, tf->type, s->bigEndian, tf->frac);
                    v.v = tf->cnt ? readComp(d + cs, tf->type, s->bigEndian, tf->frac) : 0.0f;
                }
                size = cs * (tf->cnt ? 2 : 1);
            }
            if (t == 1) s->p += size;
        }
        transformPos(m, px, py, pz, &v);
        if (lit) {
            f32 tn[3];
            tn[0] = nm[0] * nx + nm[1] * ny + nm[2] * nz;
            tn[1] = nm[3] * nx + nm[4] * ny + nm[5] * nz;
            tn[2] = nm[6] * nx + nm[7] * ny + nm[8] * nz;
            f32 len = sqrtf(tn[0] * tn[0] + tn[1] * tn[1] + tn[2] * tn[2]);
            if (len > 0.0f) { tn[0] /= len; tn[1] /= len; tn[2] /= len; }
            v.color = (port_gx_flags & 2) ? 0xFFFFFFFFu : lightColor(ch, v.color, tn, v.x, v.y, v.z);
        } else if (ch >= 0 && chan[ch].enable) {
            // lit without normals: ambient only
            static const f32 up[3] = {0.0f, 0.0f, 0.0f};
            v.color = lightColor(ch, v.color, up, v.x, v.y, v.z);
        }
        {
            f32 pos[3] = {px, py, pz}, nrm[3] = {nx, ny, nz};
            genTexCoord(&texGen[0], texMtx0, pos, nrm, v.u, v.v, &v.u, &v.v);
        }
        if (discard) continue;
        if (quads) {
            u32 q = n & 3;
            static PgVertex quad[4];
            quad[q] = v;
            if (q == 3) {
                out[o++] = quad[0]; out[o++] = quad[1]; out[o++] = quad[2];
                out[o++] = quad[0]; out[o++] = quad[2]; out[o++] = quad[3];
            }
        } else {
            out[o++] = v;
        }
    }
    (void) hasColor;
    if (discard) return s->p;
    if (cullMode == 3) return s->p;  // GX_CULL_ALL
    {
        // Non-finite or absurd coordinates (unconverted data, a bad matrix) would stall the
        // rasteriser (the emulator's software renderer walks the whole triangle): drop the draw.
        int bad = 0;
        for (u32 i = 0; i < o && !bad; i++) {
            f32 x = out[i].x, y = out[i].y, z = out[i].z;
            if (!(x == x) || !(y == y) || !(z == z) || x > 1e7f || x < -1e7f || y > 1e7f || y < -1e7f || z > 1e7f || z < -1e7f) bad = 1;
        }
        if (bad) {
            static u32 nBad;
            port_gx_stat_dropped++;
            if (++nBad <= 20) {
                port_trace("[gx] draw with non-finite / huge vertices dropped: prim %d vf %d count %u be %d mtx %d v0 %g %g %g\n", prim, vf, count, s->bigEndian, currentMtx, out[0].x, out[0].y, out[0].z);
                port_trace("[gx]   mtx %g %g %g %g / %g %g %g %g / %g %g %g %g  proj %g %g %g %g %g %g ortho %d\n", curMtx[0], curMtx[1], curMtx[2], curMtx[3], curMtx[4], curMtx[5], curMtx[6], curMtx[7], curMtx[8], curMtx[9], curMtx[10], curMtx[11],
                           projection[0][0], projection[0][2], projection[1][1], projection[1][2], projection[2][2], projection[2][3], projType);
            }
            return s->p;
        }
    }
    if ((port_gx_flags & 8) && (count == 22 || count == 4)) return s->p;  // debugger: skip the small strips / quads
    if ((port_gx_flags & 64) && count == 1024) return s->p;              // debugger: skip the screen quad grids
    if ((port_gx_flags & 2048) && count == 4 && prim != 0) return s->p;  // debugger: skip 4-vertex strips / fans
    if ((port_gx_flags & 4096) && count == 4 && prim == 0) return s->p;  // debugger: skip quads
    if ((port_gx_flags & 65536) && count == 4 && prim == 0 && !s->bigEndian) return s->p;  // debugger: skip immediate-mode quads (sprites)
    if (count == 4 && prim == 0 && !s->bigEndian) {  // debugger: bisect the immediate quads by their order in the frame
        u32 idx = immQuadIndex++;
        u32 th = ((port_gx_flags >> 20) & 0xFF) * 2;  // bits 20..27: the threshold, in pairs of quads
        if ((port_gx_flags & 0x10000000) && idx < th) return s->p;
        if ((port_gx_flags & 0x80000000u) && idx >= th) return s->p;
        if ((port_gx_flags & 0x800000) && idx < 128) {
            static u32 nTr;
            int si = textureStage();
            const TexObjPort* t = (si >= 0 && tev[si].map < 8) ? texMap[tev[si].map] : NULL;
            if (++nTr <= 128) port_trace("[gx] imm quad %u: proj %d tex %p %ux%u fmt %d col %08x blend %d %d %d v0 %g %g %g v2 %g %g %g\n", idx, projType, t ? t->data : NULL, t ? t->width : 0, t ? t->height : 0, t ? t->format : -1,
                                        out[0].color, blendType, blendSrc, blendDst, out[0].x, out[0].y, out[0].z, out[2].x, out[2].y, out[2].z);
        }
    }
    if ((port_gx_flags & 0x2000000) && colorMaskBits) return s->p;                             // debugger: skip the alpha-only passes
    if ((port_gx_flags & 0x20000000) && count == 4 && prim == 0 && textureStage() < 0) return s->p;  // debugger: skip untextured quads
    if ((port_gx_flags & 0x4000000) && blendType == 1 && blendSrc == 1 && blendDst == 1 && count == 4) return s->p;  // debugger: skip additive quads
    if (port_gx_flags & (512 | 1024 | 0x40000000)) {                      // debugger: skip draws by texture (96x96, 128x128 I8, 256x256 CMPR quads)
        int si = textureStage();
        const TexObjPort* t = (si >= 0 && tev[si].map < 8) ? texMap[tev[si].map] : NULL;
        if (t && (port_gx_flags & 512) && t->width == 96 && t->height == 96) return s->p;
        if (t && (port_gx_flags & 1024) && t->width == 128 && t->height == 128 && t->format == 1) return s->p;
        if (t && (port_gx_flags & 0x40000000) && t->width == 256 && t->height == 256 && t->format == 14 && count == 4) return s->p;
    }
    if (port_gx_flags & 128) {                                            // debugger: save the 96x96 I8 texture of a draw (as the file has it)
        int si = textureStage();
        const TexObjPort* t = (si >= 0 && tev[si].map < 8) ? texMap[tev[si].map] : NULL;
        if (t && GC_PTR_OK(t->data) && t->width == 256 && t->height == 256 && t->format == 14 && count == 4) {
            port_gx_flags &= ~128u;
            char path[80];
            sprintf(path, "ms0:/PSP/GAME/RE4/tex_%ux%u_f%d.raw", t->width, t->height, t->format);
            pg_save_raw(path, t->data, texBytes(t));
        }
    }
    if ((port_gx_flags & 32768) && count == 4 && prim == 0 && o == 6) {  // debugger: skip (and name) the quads that cover most of the screen
        f32 minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
        for (u32 i = 0; i < o; i++) {
            f32 x = out[i].x, y = out[i].y;
            if (projType == 0) { f32 d = -out[i].z; if (d < 1.0f) d = 1.0f; x = x / d * projection[0][0]; y = y / d * projection[1][1]; }
            else { x = x * projection[0][0] + projection[0][3]; y = y * projection[1][1] + projection[1][3]; }
            if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
        }
        if (maxx - minx > 1.0f && maxy - miny > 1.0f) {
            static u32 nTr;
            if (++nTr <= 30) {
                int si = textureStage();
                const TexObjPort* t = (si >= 0 && tev[si].map < 8) ? texMap[tev[si].map] : NULL;
                port_trace("[gx] big quad: ndc x %g..%g y %g..%g z %g proj %d tex %p %ux%u fmt %d col %08x blend %d %d %d\n", minx, maxx, miny, maxy, out[0].z, projType,
                           t ? t->data : NULL, t ? t->width : 0, t ? t->height : 0, t ? t->format : -1, out[0].color, blendType, blendSrc, blendDst);
            }
            return s->p;
        }
    }
    applyDrawState(hasColor);
    if ((port_gx_flags & 16384) && count == 4 && prim == 0) pg_texture_off();  // debugger: quads untextured
    if (drawConstColorOn) {
        for (u32 i = 0; i < o; i++) out[i].color = (out[i].color & 0xFF000000u) | (drawConstColor & 0x00FFFFFFu);
    }
    static const int primTbl[8] = {PG_TRIANGLES, PG_TRIANGLES, PG_TRIANGLES, PG_TRIANGLE_STRIP, PG_TRIANGLE_FAN, PG_LINES, PG_LINE_STRIP, PG_POINTS};
    if (pg_dump_pending()) {  // a frame dump was requested (pg_request_dump): describe the draws of this frame
        port_trace("[draw] prim %d vf %d count %u -> %u verts, be %d, mtx %d, pos type %d frac %d, tex %s, chan %d lit %d, list caller %p (%u bytes)\n", prim, vf, count, o,
                   s->bigEndian, currentMtx, f->pos.type, f->pos.frac, numTexGens ? "on" : "off", ch, lit, s->bigEndian ? lastListCaller : NULL, lastListBytes);
        {
            int si = textureStage();
            const TevStage* st = &tev[si < 0 ? 0 : si];
            TexObjPort* tx = st->map < 8 ? texMap[st->map] : NULL;
            port_trace("[draw]   tev stage %d/%d: cin %d %d %d %d ain %d %d %d %d op %d map %d (%s %ux%u fmt %d) texgen func %d src %d mtx %u post %u; blend %d %d %d cull %d\n",
                       si, numTevStages, st->cin[0], st->cin[1], st->cin[2], st->cin[3], st->ain[0], st->ain[1], st->ain[2], st->ain[3], st->op, st->map,
                       tx ? "tex" : "none", tx ? tx->width : 0, tx ? tx->height : 0, tx ? tx->format : -1,
                       texGen[0].func, texGen[0].src, texGen[0].mtx, texGen[0].postMtx, blendType, blendSrc, blendDst, cullMode);
            for (int k = 0; k < numTevStages && k < 16; k++) {
                if (k == si) continue;
                const TevStage* o2 = &tev[k];
                port_trace("[draw]   tev stage %d: cin %d %d %d %d ain %d %d %d %d op %d map %d coord %d kcsel %d\n", k, o2->cin[0], o2->cin[1], o2->cin[2], o2->cin[3],
                           o2->ain[0], o2->ain[1], o2->ain[2], o2->ain[3], o2->op, o2->map, o2->coord, o2->kcsel);
            }
        }
        const f32* m = curMtx;
        port_trace("[draw]   mtx %g %g %g %g / %g %g %g %g / %g %g %g %g\n", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
        const f32* pm = &projection[0][0];
        port_trace("[draw]   proj type %d: %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g; viewport %g %g %g %g %g %g\n", projType,
                   pm[0], pm[1], pm[2], pm[3], pm[4], pm[5], pm[6], pm[7], pm[8], pm[9], pm[10], pm[11], pm[12], pm[13], pm[14], pm[15],
                   viewport[0], viewport[1], viewport[2], viewport[3], viewport[4], viewport[5]);
        for (u32 i = 0; i < o && i < 4; i++) {
            port_trace("[draw]   v%u %g %g %g uv %g %g col %08x\n", i, out[i].x, out[i].y, out[i].z, out[i].u, out[i].v, out[i].color);
        }
    }
    {
        // Geometry behind the near plane: the GameCube clips it, the GE does not (a vertex with
        // w <= 0 draws garbage across the screen). Triangles are clipped here against z = -near.
        int pgPrim = primTbl[prim & 7];
        PgVertex* verts = out;
        u32 nv = o;
        if (port_profile && projType == 0 && s->bigEndian) {  // experiment: how much of a frame is wholly outside one frustum plane
            extern unsigned int port_gx_stat_offscreen, port_gx_stat_onscreen;
            u32 outL = 0, outR = 0, outT = 0, outB = 0, outF = 0;
            const f32 p00 = projection[0][0], p02 = projection[0][2], p11 = projection[1][1], p12 = projection[1][2], m22 = projection[2][2], m23 = projection[2][3];
            for (u32 i = 0; i < nv; i++) {
                const f32 w = -out[i].z, x = p00 * out[i].x + p02 * out[i].z, y = p11 * out[i].y + p12 * out[i].z;
                if (x < -w) outL++;
                if (x > w) outR++;
                if (y < -w) outB++;
                if (y > w) outT++;
                if (m22 * out[i].z + m23 > 0.0f) outF++;
            }
            if (outL == nv || outR == nv || outT == nv || outB == nv || outF == nv) port_gx_stat_offscreen += nv; else port_gx_stat_onscreen += nv;
        }
        {
            // GX clips against the z range of its clip space ([-1, 0] in the GameCube's
            // convention); the GE clamps the depth instead. A draw wholly beyond the near or the
            // far plane is dropped here: a full-screen quad placed a hair beyond the near plane
            // (the game draws such) would otherwise fill the depth buffer with "nearest" and hide
            // the scene behind it.
            const f32 m22 = projection[2][2], m23 = projection[2][3];
            u32 beforeNear = 0, beyondFar = 0;
            if (m22 == 0.0f && m23 == 0.0f) nv = 0;  // no projection loaded (the host harness): nothing to clip against
            for (u32 i = 0; i < nv; i++) {
                f32 z = out[i].z;
                f32 zc, wc;
                if (projType == 0) { zc = m22 * z + m23; wc = -z; }
                else { zc = m22 * z + m23; wc = 1.0f; }
                if (wc <= 0.0f) { beforeNear++; continue; }  // (behind the eye: the near-plane clipper below handles the perspective case)
                if (zc < -wc * 1.000001f) beforeNear++;
                else if (zc > wc * 0.000001f) beyondFar++;
            }
            if (projType == 1 && nv == 6 && (beforeNear || beyondFar)) {
                static u32 nTr;
                if (++nTr <= 20) port_trace("[gx] ortho quad z %g m22 %g m23 %g: %u before near, %u beyond far of %u -> %s\n", out[0].z, m22, m23, beforeNear, beyondFar, nv,
                                            (beforeNear == nv || beyondFar == nv) ? "dropped" : "kept");
            }
            if (nv && (beforeNear == nv || beyondFar == nv)) return s->p;
            nv = o;
        }
        if (projType == 0 && (projection[2][2] != 0.0f || projection[2][3] != 0.0f) &&
            (pgPrim == PG_TRIANGLES || pgPrim == PG_TRIANGLE_STRIP || pgPrim == PG_TRIANGLE_FAN)) {
            f32 zn = nearPlane();
            u32 behind = 0;
            for (u32 i = 0; i < nv; i++) if (out[i].z > zn) behind++;
            if (behind) {
                u32 tris = pgPrim == PG_TRIANGLES ? nv / 3 : (nv >= 3 ? nv - 2 : 0);
                PgVertex* cl = (PgVertex*) pg_get_memory((int) (tris * 6 * sizeof(PgVertex)));  // a clipped triangle is at most two
                if (!cl) return s->p;
                u32 n = 0;
                for (u32 t = 0; t < tris; t++) {
                    u32 a, b, c;
                    if (pgPrim == PG_TRIANGLES) { a = t * 3; b = a + 1; c = a + 2; }
                    else if (pgPrim == PG_TRIANGLE_STRIP) { a = t; b = t + 1; c = t + 2; if (t & 1) { u32 x = b; b = c; c = x; } }
                    else { a = 0; b = t + 1; c = t + 2; }
                    n += clipTriangleNear(&out[a], &out[b], &out[c], zn, cl + n);
                }
                if (n == 0) return s->p;
                pgPrim = PG_TRIANGLES; verts = cl; nv = n;
            }
        }
        pg_draw(pgPrim, (int) nv, verts);
    }
    return s->p;
}

// ---------------------------------------------------------------- write-gather pipe (immediate mode)
#define IMM_MAX 0x20000
static u8 immBuf[IMM_MAX];
static u32 immLen;
static int immActive;
static int immPrim, immFmt;
static u32 immCount;
static u32 immBytesNeeded;

// Recording (GXBeginDisplayList): the draws land in the game's buffer in the port's own format.
static u8* recBuf;
static u32 recLen, recMax;
#define REC_MAGIC 0x50444C31u  // 'PDL1'

static u32 directVertexSize(int vf)
{
    const Vat* f = &vat[vf & 7];
    u32 n = 0;
    for (int a = 0; a < A_COUNT; a++) {
        if (vcd[a] != 1) continue;
        if (a == A_PNMTX || (a >= A_TEXMTX0 && a < A_POS)) n += 1;
        else if (a == A_POS) n += compSize(f->pos.type) * (f->pos.cnt ? 3 : 2);
        else if (a == A_NRM) n += compSize(f->nrm.type) * (f->nrm.cnt ? 9 : 3);
        else if (a == A_CLR0 || a == A_CLR1) {
            static const u8 cs[6] = {2, 3, 4, 2, 3, 4};
            n += cs[f->clr[a - A_CLR0].type % 6];
        } else n += compSize(f->tex[a - A_TEX0].type) * (f->tex[a - A_TEX0].cnt ? 2 : 1);
    }
    for (int a = 0; a < A_COUNT; a++) {
        if (vcd[a] == 2) n += 1;
        else if (vcd[a] == 3) n += 2;
    }
    return n;
}

static void immFlush(void)
{
    if (!immActive) return;
    immActive = 0;
    if (recBuf) {
        // record: marker, prim, fmt, count, vcd, vat, bytes
        u32 need = 1 + 1 + 1 + 4 + A_COUNT + sizeof(Vat) + immLen;
        if (recLen + need <= recMax) {
            u8* d = recBuf + recLen;
            d[0] = 0xFF; d[1] = (u8) immPrim; d[2] = (u8) immFmt;
            memcpy(d + 3, &immCount, 4);
            memcpy(d + 7, vcd, A_COUNT);
            memcpy(d + 7 + A_COUNT, &vat[immFmt & 7], sizeof(Vat));
            memcpy(d + 7 + A_COUNT + sizeof(Vat), immBuf, immLen);
            recLen += need;
        }
        return;
    }
    Parser s = {immBuf, 0};
    drawVertices(&s, immPrim, immFmt, immCount);
}

static inline void immPut(const void* d, u32 n)
{
    if (!immActive || immLen + n > IMM_MAX) return;
    memcpy(immBuf + immLen, d, n);
    immLen += n;
    if (immLen >= immBytesNeeded) immFlush();
}

extern "C" void port_gx_put_u8(u8 v) { immPut(&v, 1); }
extern "C" void port_gx_put_s8(s8 v) { immPut(&v, 1); }
extern "C" void port_gx_put_u16(u16 v) { immPut(&v, 2); }
extern "C" void port_gx_put_s16(s16 v) { immPut(&v, 2); }
extern "C" void port_gx_put_u32(u32 v) { immPut(&v, 4); }
extern "C" void port_gx_put_f32(f32 v) { immPut(&v, 4); }

// ---------------------------------------------------------------- the GX API
extern "C" {

void GXInit_port(void)
{
    if (inited) return;
    pg_init();
    inited = 1;
    for (int i = 0; i < 64; i++) {
        mtxMem[i][0] = (i % 3 == 0) ? 1.0f : 0.0f;
        mtxMem[i][1] = (i % 3 == 1) ? 1.0f : 0.0f;
        mtxMem[i][2] = (i % 3 == 2) ? 1.0f : 0.0f;
        mtxMem[i][3] = 0.0f;
    }
    for (int i = 0; i < 16; i++) { tev[i].coord = 0; tev[i].map = 0xFF; tev[i].color = 0xFF; tev[i].op = 0; }
    tev[0].map = 0;
    tev[0].color = 4;  // GXInit: stage 0 rasterises GX_COLOR0A0
    GXSetTevOp(0, 0);  // GX_MODULATE
    for (int i = 0; i < 64; i++) {
        nrmMem[i][0] = (i % 3 == 0) ? 1.0f : 0.0f;
        nrmMem[i][1] = (i % 3 == 1) ? 1.0f : 0.0f;
        nrmMem[i][2] = (i % 3 == 2) ? 1.0f : 0.0f;
    }
    for (int i = 0; i < 8; i++) { texGen[i].func = 1; texGen[i].src = 4; texGen[i].normalize = 0; texGen[i].mtx = 60; texGen[i].postMtx = 125; }
    viewport[0] = 0; viewport[1] = 0; viewport[2] = efbW; viewport[3] = efbH; viewport[4] = 0; viewport[5] = 1;
}

// GXInit(base, size) returns the FIFO object; the port has no FIFO.
static u8 fakeFifo[0x80];
void* GXInit(void* base, u32 size)
{
    GXInit_port();
    return fakeFifo;
}

// --- frame
void GXSetCopyClear(GXColor color, u32 z)
{
    copyClearColor = ((u32) color.a << 24) | ((u32) color.b << 16) | ((u32) color.g << 8) | color.r;
    copyClearZ = z;
}

static const void* dlSeen[4096];
static const void* dlCaller[4096];
static u32 dlFirst[4096];
static u32 dlCalls, dlDistinct, dlBytes, dlDistinctBytes;
void GXCopyDisp(void* dest, u8 clear)
{
    if (!inited) GXInit_port();
    immFlush();
    pg_finish();
    flushDeferredFree();  // the GE is done with the frame's textures
    immQuadIndex = 0;
    if (port_profile) {
        static u32 nf;
        if ((++nf & 127) == 0) port_trace("[gx] display lists this frame: %u calls (%u KB), %u distinct (%u KB)\n", dlCalls, dlBytes / 1024, dlDistinct, dlDistinctBytes / 1024);
        memset(dlSeen, 0, sizeof(dlSeen));
        dlCalls = dlDistinct = dlBytes = dlDistinctBytes = 0;
    }
    pg_swap();
    drawOverlay();
    pg_start();
    if (clear) {
        pg_clear(copyClearColor, 65535, 1, 1);
    }
    applyViewport();
    applyProjection();
}

void GXDrawDone(void)
{
    if (!inited) return;
    immFlush();
    pg_finish();
    pg_start();
}

void GXPixModeSync(void) {}
void GXFlush(void) {}
void GXSetDrawSync(u16 token) {}
void* GXSetDrawSyncCallback(void* cb) { return NULL; }
void* GXSetCurrentGXThread(void) { return NULL; }

// --- viewport, projection, matrices
void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz)
{
    viewport[0] = left; viewport[1] = top; viewport[2] = wd; viewport[3] = ht; viewport[4] = nearz; viewport[5] = farz;
    if (inited) applyViewport();
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field)
{
    GXSetViewport(left, top, wd, ht, nearz, farz);
}

void GXGetViewportv(f32* vp) { memcpy(vp, viewport, sizeof(viewport)); }

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht)
{
    if (!inited) return;
    pg_scissor((int) (left * sx()), (int) (top * sy()), (int) (wd * sx() + 0.5f), (int) (ht * sy() + 0.5f));
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht)
{
    if (wd && ht) { efbW = wd; efbH = ht; }
}

void GXSetProjection(const f32 mtx[4][4], int type)
{
    memcpy(projection, mtx, sizeof(projection));
    projType = type;
    if (inited) applyProjection();
}

void GXGetProjectionv(f32* p)
{
    p[0] = (f32) projType;
    p[1] = projection[0][0]; p[2] = projection[0][2]; p[3] = projection[1][1]; p[4] = projection[1][2];
    p[5] = projection[2][2]; p[6] = projection[2][3];
}

void GXLoadPosMtxImm(const f32 mtx[3][4], u32 id)
{
    if (id + 2 < 64) memcpy(&mtxMem[id][0], mtx, 12 * sizeof(f32));
    {
        static u32 nNan;
        const f32* m = &mtx[0][0];
        int bad = 0;
        for (int i = 0; i < 12; i++) if (!(m[i] == m[i])) bad = 1;
        if (bad && ++nNan <= 8) port_trace("[gx] NaN position matrix %u loaded from %p\n", (unsigned) id, __builtin_return_address(0));
    }
}

void GXLoadNrmMtxImm(const f32 mtx[3][4], u32 id)
{
    if (id + 2 < 64) {
        for (int r = 0; r < 3; r++) {
            nrmMem[id + r][0] = mtx[r][0]; nrmMem[id + r][1] = mtx[r][1]; nrmMem[id + r][2] = mtx[r][2];
        }
    }
}

void GXLoadTexMtxImm(const f32 mtx[][4], u32 id, int type)
{
    if (id + 2 < 64) memcpy(&mtxMem[id][0], mtx, (type == 0 ? 12 : 8) * sizeof(f32));
    else if (id >= 64 && id + 2 < 128) memcpy(&ptMem[id - 64][0], mtx, (type == 0 ? 12 : 8) * sizeof(f32));
}

void GXSetCurrentMtx(u32 id) { currentMtx = id < 64 ? id : 0; }

// --- vertex descriptors and arrays
void GXClearVtxDesc(void) { memset(vcd, 0, sizeof(vcd)); }

void GXSetVtxDesc(int attr, int type)
{
    if (attr >= 0 && attr < A_COUNT) vcd[attr] = (u8) type;
    if (attr == 25) vcd[A_NRM] = (u8) type;  // GX_VA_NBT
}

void GXSetVtxAttrFmt(int vtxfmt, int attr, int cnt, int type, u8 frac)
{
    Vat* f = &vat[vtxfmt & 7];
    Fmt fm = {(u8) cnt, (u8) type, frac};
    if (attr == A_POS) f->pos = fm;
    else if (attr == A_NRM || attr == 25) { f->nrm = fm; if (attr == 25) f->nrm.cnt = 1; }
    else if (attr == A_CLR0) f->clr[0] = fm;
    else if (attr == A_CLR1) f->clr[1] = fm;
    else if (attr >= A_TEX0 && attr < A_TEX0 + 8) f->tex[attr - A_TEX0] = fm;
}

void GXSetArray(int attr, void* base_ptr, u8 stride)
{
    if (attr == 25) attr = A_NRM;
    if (attr >= 0 && attr < A_COUNT) { arrayBase[attr] = (const u8*) base_ptr; arrayStride[attr] = stride; }
}

void GXInvalidateVtxCache(void) {}

// --- immediate mode and display lists
void GXBegin(int type, int vtxfmt, u16 nverts)
{
    if (!inited) GXInit_port();
    immFlush();
    immActive = 1;
    immLen = 0;
    immPrim = (type >> 3) & 7;
    immFmt = vtxfmt & 7;
    immCount = nverts;
    immBytesNeeded = directVertexSize(immFmt) * nverts;
    if (immBytesNeeded == 0 || immBytesNeeded > IMM_MAX) immActive = 0;
}


void GXBeginDisplayList(void* list, u32 size)
{
    immFlush();
    recBuf = (u8*) list;
    recMax = size;
    recLen = 0;
    if (size >= 4) { memcpy(recBuf, "1LDP", 4); recLen = 4; }  // REC_MAGIC little-endian
}

u32 GXEndDisplayList(void)
{
    immFlush();
    u32 n = recLen;
    recBuf = NULL;
    return (n + 31) & ~31u;
}

static void callRecorded(const u8* p, const u8* end)
{
    while (p + 7 + A_COUNT + sizeof(Vat) <= end && *p == 0xFF) {
        int prim = p[1], fmt = p[2];
        u32 count;
        memcpy(&count, p + 3, 4);
        u8 savedVcd[A_COUNT];
        Vat savedVat = vat[fmt & 7];
        memcpy(savedVcd, vcd, A_COUNT);
        memcpy(vcd, p + 7, A_COUNT);
        memcpy(&vat[fmt & 7], p + 7 + A_COUNT, sizeof(Vat));
        Parser s = {p + 7 + A_COUNT + sizeof(Vat), 0};
        const u8* next = drawVertices(&s, prim, fmt, count);
        memcpy(vcd, savedVcd, A_COUNT);
        vat[fmt & 7] = savedVat;
        p = next;
    }
}

void GXCallDisplayList(void* list, u32 nbytes)
{
    if (!inited) GXInit_port();
    lastListCaller = __builtin_return_address(0);
    lastListBytes = nbytes;
    immFlush();
    if (nbytes > 0x400000 || list == NULL) {  // a size from an unconverted header: walking it would take seconds
        static u32 nb;
        if (++nb <= 20) port_trace("[gx] display list %p of %u bytes refused\n", list, (unsigned) nbytes);
        return;
    }
    if (port_profile) {  // experiment: is a display list drawn more than once a frame?
        u32 h = (((u32) (uintptr_t) list) >> 5) & 4095;
        dlCalls++; dlBytes += nbytes;
        while (dlSeen[h] && dlSeen[h] != list) h = (h + 1) & 4095;
        if (!dlSeen[h]) { dlSeen[h] = list; dlDistinct++; dlDistinctBytes += nbytes; dlFirst[h] = dlCalls; dlCaller[h] = lastListCaller; }
        else {
            static u32 nTr;
            if (nbytes > 4096 && ++nTr <= 24) {
                int si = textureStage();
                port_trace("[gx] list %p (%u bytes) again: call %u from %p, first was call %u from %p; blend %d %d %d cull %d tev stages %d tex stage %d colour mask %x alpha mask %x mtx %g %g %g\n", list, (unsigned) nbytes, dlCalls, lastListCaller, dlFirst[h], dlCaller[h],
                           blendType, blendSrc, blendDst, cullMode, numTevStages, si, (unsigned) colorMaskBits, (unsigned) alphaMaskBits, mtxMem[currentMtx][3], mtxMem[currentMtx][7], mtxMem[currentMtx][11]);
            }
        }
    }
    const u8* p = (const u8*) list;
    const u8* end = p + nbytes;
    if (nbytes >= 4 && memcmp(p, "1LDP", 4) == 0) {
        callRecorded(p + 4, end);
        return;
    }
    Parser s = {p, 1};
    while (s.p < end) {
        u8 op = *s.p++;
        if (op == 0x00) continue;
        if (op == 0x08) {  // CP register
            u8 reg = *s.p;
            u32 val = be32(s.p + 1);
            s.p += 5;
            if (reg == 0x50) {
                vcd[A_PNMTX] = val & 1;
                for (int i = 0; i < 8; i++) vcd[A_TEXMTX0 + i] = (val >> (1 + i)) & 1;
                vcd[A_POS] = (val >> 9) & 3; vcd[A_NRM] = (val >> 11) & 3; vcd[A_CLR0] = (val >> 13) & 3; vcd[A_CLR1] = (val >> 15) & 3;
            } else if (reg == 0x60) {
                for (int i = 0; i < 8; i++) vcd[A_TEX0 + i] = (val >> (2 * i)) & 3;
            } else if (reg >= 0x70 && reg <= 0x77) {
                Vat* f = &vat[reg - 0x70];
                f->pos.cnt = val & 1; f->pos.type = (val >> 1) & 7; f->pos.frac = (val >> 4) & 31;
                f->nrm.cnt = (val >> 9) & 1; f->nrm.type = (val >> 10) & 7; f->nrm.frac = 0;
                f->clr[0].cnt = (val >> 13) & 1; f->clr[0].type = (val >> 14) & 7;
                f->clr[1].cnt = (val >> 17) & 1; f->clr[1].type = (val >> 18) & 7;
                f->tex[0].cnt = (val >> 21) & 1; f->tex[0].type = (val >> 22) & 7; f->tex[0].frac = (val >> 25) & 31;
            } else if (reg >= 0x80 && reg <= 0x87) {
                Vat* f = &vat[reg - 0x80];
                f->tex[1].cnt = val & 1; f->tex[1].type = (val >> 1) & 7; f->tex[1].frac = (val >> 4) & 31;
                f->tex[2].cnt = (val >> 9) & 1; f->tex[2].type = (val >> 10) & 7; f->tex[2].frac = (val >> 13) & 31;
                f->tex[3].cnt = (val >> 18) & 1; f->tex[3].type = (val >> 19) & 7; f->tex[3].frac = (val >> 22) & 31;
            } else if (reg >= 0xB0 && reg <= 0xBB) {
                int a = reg - 0xB0;
                if (a < 12) arrayStride[a == 0 ? A_POS : a == 1 ? A_NRM : a == 2 ? A_CLR0 : a == 3 ? A_CLR1 : A_TEX0 + a - 4] = val & 0xFF;
            }
            continue;
        }
        if (op == 0x10) {  // XF register load
            u32 n = (be16(s.p) & 0xF) + 1;
            s.p += 4 + n * 4;
            continue;
        }
        if (op == 0x61) { s.p += 4; continue; }             // BP register
        if (op >= 0x20 && op <= 0x38 && (op & 7) == 0) { s.p += 4; continue; }  // indexed XF loads
        if (op == 0x40) continue;
        if (op == 0x48) { s.p += 8; continue; }
        if (op >= 0x80) {
            int prim = (op >> 3) & 7;
            int vf = op & 7;
            u32 count = be16(s.p);
            s.p += 2;
            s.p = drawVertices(&s, prim, vf, count);
            continue;
        }
        break;  // unknown opcode: stop
    }
}

// --- render state
void GXSetZMode(u8 compare_enable, int func, u8 update_enable)
{
    if (!inited) return;
    pg_depth(compare_enable, compareFunc(func), update_enable);
}

void GXSetZCompLoc(u8 before_tex) {}

// The blend the GE gets: the game's blend mode, or, while the colour update is off (the filters
// write the frame's alpha alone: filter03), a blend that leaves the colour as it is. The GE
// writes the source alpha whatever the blend, so the alpha-only pass still lands; the pixel mask
// alone is not honoured everywhere.
static void applyBlend(void)
{
    if (!inited) return;
    if (colorMaskBits) {
        pg_blend(1, PG_ADD, PG_FIX, PG_FIX, 0x000000, 0xFFFFFF);
        return;
    }
    static const int srcTbl[8] = {PG_FIX, PG_FIX, PG_SRC_COLOR, PG_ONE_MINUS_SRC_COLOR, PG_SRC_ALPHA, PG_ONE_MINUS_SRC_ALPHA, PG_DST_ALPHA, PG_ONE_MINUS_DST_ALPHA};
    static const int dstTbl[8] = {PG_FIX, PG_FIX, PG_DST_COLOR, PG_ONE_MINUS_DST_COLOR, PG_SRC_ALPHA, PG_ONE_MINUS_SRC_ALPHA, PG_DST_ALPHA, PG_ONE_MINUS_DST_ALPHA};
    u32 fixS = blendSrc == 1 ? 0xFFFFFF : 0;
    u32 fixD = blendDst == 1 ? 0xFFFFFF : 0;
    if (blendType == 1) {        // GX_BM_BLEND
        pg_blend(1, PG_ADD, srcTbl[blendSrc & 7], dstTbl[blendDst & 7], fixS, fixD);
    } else if (blendType == 3) { // GX_BM_SUBTRACT: dst - src
        pg_blend(1, PG_REVERSE_SUBTRACT, PG_FIX, PG_FIX, 0xFFFFFF, 0xFFFFFF);
    } else {
        pg_blend(0, 0, 0, 0, 0, 0);
    }
}

void GXSetBlendMode(int type, int src_factor, int dst_factor, int op)
{
    blendType = type; blendSrc = src_factor; blendDst = dst_factor;
    applyBlend();
}

void GXSetCullMode(int mode)
{
    cullMode = mode;
    if (!inited) return;
    // GX front faces are clockwise on screen; GX_CULL_BACK keeps them.
    if (mode == 1) pg_cull(1, 0);       // GX_CULL_FRONT
    else if (mode == 2) pg_cull(1, 1);  // GX_CULL_BACK
    else pg_cull(0, 0);
}

void GXSetAlphaCompare(int comp0, u8 ref0, int op, int comp1, u8 ref1)
{
    if (!inited) return;
    if (comp0 == 7) pg_alpha_test(0, 0, 0);
    else pg_alpha_test(1, compareFunc(comp0), ref0);
}

void GXSetColorUpdate(u8 enable)
{
    colorMaskBits = enable ? 0 : 0x00FFFFFF;
    if (inited) pg_pixel_mask(colorMaskBits | alphaMaskBits);
    applyBlend();
}

void GXSetAlphaUpdate(u8 enable)
{
    alphaMaskBits = enable ? 0 : 0xFF000000;
    if (inited) pg_pixel_mask(colorMaskBits | alphaMaskBits);
}

void GXSetFog(int type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color)
{
    if (!inited) return;
    u32 c = ((u32) color.b << 16) | ((u32) color.g << 8) | color.r;
    pg_fog(type != 0, startz, endz, c);
}

void GXSetDither(u8 dither) {}
void GXSetLineWidth(u8 width, int tex_offsets) {}
void GXSetDstAlpha(u8 enable, u8 alpha) {}
void GXSetPixelFmt(int pix_fmt, int z_fmt) {}
void GXSetCopyFilter(u8 aa, const u8 sample_pattern[12][2], u8 vf, const u8 vfilter[7]) {}
void GXSetDispCopyGamma(int gamma) {}
void GXSetDispCopyDst(u16 wd, u16 ht) {}
u32 GXSetDispCopyYScale(f32 vscale) { return (u32) (efbH * vscale); }
void GXSetClipMode(int mode) {}
void GXClearBoundingBox(void) {}

// --- TEV / texgen / channels
void GXSetNumTevStages(u8 n) { numTevStages = n; }
void GXSetNumTexGens(u8 n) { numTexGens = n; }
void GXSetNumChans(u8 n) { numChans = n; }

void GXSetTevOrder(int stage, int coord, int map, int color)
{
    if (stage >= 0 && stage < 16) { tev[stage].coord = (u8) coord; tev[stage].map = (u8) map; tev[stage].color = (u8) color; }
}

void GXSetTevColorIn(int stage, int a, int b, int c, int d)
{
    if (stage < 0 || stage >= 16) return;
    tev[stage].cin[0] = (u8) a; tev[stage].cin[1] = (u8) b; tev[stage].cin[2] = (u8) c; tev[stage].cin[3] = (u8) d;
}
void GXSetTevAlphaIn(int stage, int a, int b, int c, int d)
{
    if (stage < 0 || stage >= 16) return;
    tev[stage].ain[0] = (u8) a; tev[stage].ain[1] = (u8) b; tev[stage].ain[2] = (u8) c; tev[stage].ain[3] = (u8) d;
}
// The SDK's GXSetTevOp is the combiner inputs of the five classic modes.
void GXSetTevOp(int stage, int mode)
{
    if (stage < 0 || stage >= 16) return;
    tev[stage].op = (u8) mode;
    switch (mode) {
    case 0: GXSetTevColorIn(stage, 15, 8, 10, 15); GXSetTevAlphaIn(stage, 7, 4, 5, 7); break;   // GX_MODULATE
    case 1: GXSetTevColorIn(stage, 10, 8, 9, 15); GXSetTevAlphaIn(stage, 7, 7, 7, 5); break;    // GX_DECAL
    case 2: GXSetTevColorIn(stage, 10, 12, 8, 15); GXSetTevAlphaIn(stage, 7, 4, 5, 7); break;   // GX_BLEND
    case 3: GXSetTevColorIn(stage, 15, 15, 15, 8); GXSetTevAlphaIn(stage, 7, 7, 7, 4); break;   // GX_REPLACE
    default: GXSetTevColorIn(stage, 15, 15, 15, 10); GXSetTevAlphaIn(stage, 7, 7, 7, 5); break; // GX_PASSCLR
    }
}
void GXSetTevColorOp(int stage, int op, int bias, int scale, u8 clamp, int out_reg) {}
void GXSetTevAlphaOp(int stage, int op, int bias, int scale, u8 clamp, int out_reg) {}
void GXSetTevColor(int id, GXColor color)
{
    if (id >= 0 && id < 4) tevRegColor[id] = ((u32) color.a << 24) | ((u32) color.b << 16) | ((u32) color.g << 8) | color.r;
}
void GXSetTevColorS10(int id, GXColorS10 color) {}
void GXSetTevKColor(int id, GXColor color)
{
    if (id >= 0 && id < 4) tevKColor[id] = ((u32) color.a << 24) | ((u32) color.b << 16) | ((u32) color.g << 8) | color.r;
}
void GXSetTevKColorSel(int stage, int sel) { if (stage >= 0 && stage < 16) tev[stage].kcsel = (u8) sel; }
void GXSetTevKAlphaSel(int stage, int sel) {}
void GXSetTevSwapMode(int stage, int ras_sel, int tex_sel) {}
void GXSetTevSwapModeTable(int table, int red, int green, int blue, int alpha) {}
void GXSetTevDirect(int stage) {}
void GXSetNumIndStages(u8 n) {}
void GXSetIndTexOrder(int stage, int coord, int map) {}
void GXSetIndTexCoordScale(int stage, int s, int t) {}
void GXSetIndTexMtx(int id, const f32 offset[2][3], s8 scale_exp) {}
void GXSetTevIndWarp(int stage, int ind_stage, u8 signed_offsets, u8 replace_mode, int matrix_sel) {}
void GXSetTevIndBumpXYZ(int stage, int ind_stage, int matrix_sel) {}
void GXEnableTexOffsets(int coord, u8 line, u8 point) {}

void GXSetTexCoordGen2(int dst, int func, int src, u32 mtx, u8 normalize, u32 pt)  // GXSetTexCoordGen is gx.h's macro over it
{
    if (dst < 0 || dst >= 8) return;
    texGen[dst].func = (u8) func; texGen[dst].src = (u8) src; texGen[dst].normalize = normalize;
    texGen[dst].mtx = mtx; texGen[dst].postMtx = pt;
}

// GX_COLOR0 / GX_COLOR1 / GX_ALPHA0 / GX_ALPHA1 (0..3) set one channel; GX_COLOR0A0 / GX_COLOR1A1
// (4, 5) set the colour channel and its alpha channel together (the effects and the HUD use those).
void GXSetChanCtrl(int ch, u8 enable, int amb_src, int mat_src, u32 light_mask, int diff_fn, int attn_fn)
{
    int first, last;
    lightGen++;
    if (ch >= 0 && ch <= 3) { first = last = ch; }
    else if (ch == 4) { first = 0; last = 2; }
    else if (ch == 5) { first = 1; last = 3; }
    else return;
    for (int i = first; i <= last; i += 2) {
        Chan* c = &chan[i];
        c->enable = enable; c->ambSrc = (u8) amb_src; c->matSrc = (u8) mat_src;
        c->lightMask = light_mask; c->diffFn = (u8) diff_fn; c->attnFn = (u8) attn_fn;
    }
}
// GX_COLOR0 / GX_ALPHA0 / GX_COLOR0A0 (0, 2, 4) set pair 0; GX_COLOR1 / GX_ALPHA1 / GX_COLOR1A1 pair 1.
void GXSetChanMatColor(int ch, GXColor color)
{
    lightGen++;
    u32 c = ((u32) color.a << 24) | ((u32) color.b << 16) | ((u32) color.g << 8) | color.r;
    if (ch == 0 || ch == 2 || ch == 4) chanMatColor[0] = c; else chanMatColor[1] = c;
}
void GXSetChanAmbColor(int ch, GXColor color)
{
    lightGen++;
    u32 c = ((u32) color.a << 24) | ((u32) color.b << 16) | ((u32) color.g << 8) | color.r;
    if (ch == 0 || ch == 2 || ch == 4) chanAmbColor[0] = c; else chanAmbColor[1] = c;
}
static inline LightPort* lightOf(GXLightObj* obj) { return (LightPort*) obj; }
void GXInitLightColor(GXLightObj* obj, GXColor color)
{
    lightOf(obj)->color = ((u32) color.a << 24) | ((u32) color.b << 16) | ((u32) color.g << 8) | color.r;
}
void GXInitLightPos(GXLightObj* obj, f32 x, f32 y, f32 z) { LightPort* l = lightOf(obj); l->px = x; l->py = y; l->pz = z; }
void GXInitLightDir(GXLightObj* obj, f32 x, f32 y, f32 z) { LightPort* l = lightOf(obj); l->dx = x; l->dy = y; l->dz = z; }
void GXInitLightAttn(GXLightObj* obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2)
{
    LightPort* l = lightOf(obj);
    l->a0 = a0; l->a1 = a1; l->a2 = a2; l->k0 = k0; l->k1 = k1; l->k2 = k2;
}
void GXInitLightAttnK(GXLightObj* obj, f32 k0, f32 k1, f32 k2) { LightPort* l = lightOf(obj); l->k0 = k0; l->k1 = k1; l->k2 = k2; }
// The SDK's spot / distance attenuation coefficient tables (GXLight.c).
void GXInitLightSpot(GXLightObj* obj, f32 cutoff, int spot_fn)
{
    LightPort* l = lightOf(obj);
    f32 a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
    if (cutoff > 0.0f && cutoff <= 90.0f) {
        f32 cr = cosf(cutoff * 3.14159265f / 180.0f);
        f32 d = (1.0f - cr) * (1.0f - cr);
        switch (spot_fn) {
        case 1: a0 = -1000.0f * cr; a1 = 1000.0f; a2 = 0.0f; break;                          // GX_SP_FLAT
        case 2: a0 = -cr / (1.0f - cr); a1 = 1.0f / (1.0f - cr); a2 = 0.0f; break;           // GX_SP_COS
        case 3: a0 = 0.0f; a1 = -cr / (1.0f - cr); a2 = 1.0f / (1.0f - cr); break;           // GX_SP_COS2
        case 4: a0 = cr * (cr - 2.0f) / d; a1 = 2.0f / d; a2 = -1.0f / d; break;             // GX_SP_SHARP
        case 5: a0 = -4.0f * cr / d; a1 = 4.0f * (1.0f + cr) / d; a2 = -4.0f / d; break;     // GX_SP_RING1
        case 6: a0 = 1.0f - 2.0f * cr * cr / d; a1 = 4.0f * cr / d; a2 = -2.0f / d; break;   // GX_SP_RING2
        default: break;                                                                     // GX_SP_OFF
        }
    }
    l->a0 = a0; l->a1 = a1; l->a2 = a2;
}
void GXInitLightDistAttn(GXLightObj* obj, f32 ref_dist, f32 ref_br, int dist_fn)
{
    LightPort* l = lightOf(obj);
    f32 k0 = 1.0f, k1 = 0.0f, k2 = 0.0f;
    if (ref_dist >= 0.0f && ref_br > 0.0f && ref_br < 1.0f) {
        switch (dist_fn) {
        case 1: k1 = (1.0f - ref_br) / (ref_br * ref_dist); break;                                  // GX_DA_GENTLE
        case 2: k1 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist); k2 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist * ref_dist); break;  // GX_DA_MEDIUM
        case 3: k2 = (1.0f - ref_br) / (ref_br * ref_dist * ref_dist); break;                       // GX_DA_STEEP
        default: break;                                                                             // GX_DA_OFF
        }
    }
    l->k0 = k0; l->k1 = k1; l->k2 = k2;
}
void GXLoadLightObjImm(GXLightObj* obj, u32 light)
{
    lightGen++;
    for (int i = 0; i < 8; i++) {
        if (light & (1u << i)) lights[i] = *lightOf(obj);
    }
}

// --- textures
void GXInitTexObj(GXTexObj* obj, void* image, u16 width, u16 height, int format, int wrap_s, int wrap_t, u8 mipmap)
{
    TexObjPort* t = (TexObjPort*) obj;
    memset(t, 0, sizeof(*t));
    t->data = image; t->width = width; t->height = height; t->format = (u8) format;
    t->wrapS = (u8) wrap_s; t->wrapT = (u8) wrap_t; t->mipmap = mipmap;
    t->minFilt = 1; t->magFilt = 1;
}

void GXInitTexObjCI(GXTexObj* obj, void* image, u16 width, u16 height, int format, int wrap_s, int wrap_t, u8 mipmap, u32 tlut_name)
{
    GXInitTexObj(obj, image, width, height, format, wrap_s, wrap_t, mipmap);
    TexObjPort* t = (TexObjPort*) obj;
    t->isCI = 1;
    t->tlutName = tlut_name;
}

void GXInitTexObjLOD(GXTexObj* obj, int min_filt, int mag_filt, f32 min_lod, f32 max_lod, f32 lod_bias, u8 bias_clamp, u8 do_edge_lod, int max_aniso)
{
    TexObjPort* t = (TexObjPort*) obj;
    t->minFilt = (u8) (min_filt & 1);  // GX_NEAR / GX_LINEAR and their mip variants
    t->magFilt = (u8) (mag_filt & 1);
}

void* GXGetTexObjData(GXTexObj* obj) { return (void*) ((TexObjPort*) obj)->data; }

void GXInitTlutObj(GXTlutObj* obj, void* lut, int fmt, u16 n_entries)
{
    TlutPort* t = (TlutPort*) obj;
    t->lut = lut; t->fmt = (u16) fmt; t->entries = n_entries;
}

void GXLoadTlut(GXTlutObj* obj, u32 tlut_name)
{
    if (tlut_name < 20) {
        tlutTable[tlut_name] = *(const TlutPort*) obj;
        tlutLoaded[tlut_name] = 1;
    }
}

void GXLoadTexObj(GXTexObj* obj, int id)
{
    if (id >= 0 && id < 8) texMap[id] = (TexObjPort*) obj;
}

void GXInvalidateTexAll(void) { invalidateTextures(); }

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod)
{
    u32 w = (width + 7) & ~7u, h = (height + 7) & ~7u;
    switch (format) {
    case 0: case 8: case 14: return w * h / 2;
    case 1: case 2: case 9: return w * h;
    case 6: return w * h * 4;
    default: return w * h * 2;
    }
}

// --- framebuffer copies (not reproduced: the destination keeps whatever it held)
void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht)
{
    copySrc[0] = left; copySrc[1] = top; copySrc[2] = wd; copySrc[3] = ht;
}
void GXSetTexCopyDst(u16 wd, u16 ht, int fmt, u8 mipmap)
{
    copyDstW = wd; copyDstH = ht; copyDstFmt = (u8) fmt;
}
void GXCopyTex(void* dest, u8 clear)
{
    if (!inited) GXInit_port();
    immFlush();
    CopyRecord* r = NULL;
    for (int i = 0; i < COPY_MAX; i++) {
        if (copies[i].buf && copies[i].dest == dest) { r = &copies[i]; break; }
    }
    if (r && (r->w != copyDstW || r->h != copyDstH)) {
        free(r->buf);
        r->buf = NULL;
        r = NULL;
    }
    if (!r) {
        for (int i = 0; i < COPY_MAX; i++) {
            if (!copies[i].buf) { r = &copies[i]; break; }
        }
        if (!r) {  // recycle the oldest slot
            r = &copies[0];
            free(r->buf);
            r->buf = NULL;
        }
        if (copyDstW == 0 || copyDstH == 0) return;
        u32 pw = pow2ceil(copyDstW), ph = pow2ceil(copyDstH);
        if (pw > 512) pw = 512;
        if (ph > 512) ph = 512;
        r->buf = (u32*) ALIGNED_ALLOC(pw * ph * 4);
        if (!r->buf) return;
        r->dest = dest; r->w = copyDstW; r->h = copyDstH; r->pw = (u16) pw; r->ph = (u16) ph;
        memset(texMemo, 0, sizeof(texMemo));  // (a texture at that address is the copy from now on)
    }
    // GX_CTF_A8 (0x27) keeps the alpha, the Z formats (GX_TF_Z8 0x11 .. GX_TF_Z24X8 0x16) the depth
    int mode = copyDstFmt == 0x27 ? 1 : (copyDstFmt >= 0x11 && copyDstFmt <= 0x16) ? 2 : 0;
    port_gx_stat_copies++;
    {
        static u32 nTr;
        if (pg_dump_pending() || ++nTr <= 12)
            port_trace("[gx] GXCopyTex dest %p fmt %02x %ux%u (buffer %ux%u) from %u,%u %ux%u mode %d clear %d\n", dest, copyDstFmt, copyDstW, copyDstH, r->pw, r->ph,
                       (unsigned) copySrc[0], (unsigned) copySrc[1], (unsigned) copySrc[2], (unsigned) copySrc[3], mode, clear);
    }
    pg_texture_forget();
    pg_copy_frame(r->buf, r->pw, r->ph, (int) (copySrc[0] * sx()), (int) (copySrc[1] * sy()),
                  (int) (copySrc[2] * sx()), (int) (copySrc[3] * sy()), mode);
    if ((port_gx_flags & 16) && r->pw == 64) {  // debugger: save the 64x64 (alpha) copy buffer (8888)
        port_gx_flags &= ~16u;
        char path[64];
        sprintf(path, "ms0:/PSP/GAME/RE4/copy_%ux%u.raw", r->pw, r->ph);
        pg_save_raw(path, r->buf, r->pw * r->ph * 4);
    }
    if (clear) {
        pg_clear(copyClearColor, 65535, 1, 1);
    }
}
void GXPeekZ(u16 x, u16 y, u32* z) { *z = 0xFFFFFF; }

// --- misc
void GXProject(f32 x, f32 y, f32 z, const f32 mtx[3][4], const f32* pm, const f32* vp, f32* sx_, f32* sy_, f32* sz_)
{
    f32 ex = mtx[0][0] * x + mtx[0][1] * y + mtx[0][2] * z + mtx[0][3];
    f32 ey = mtx[1][0] * x + mtx[1][1] * y + mtx[1][2] * z + mtx[1][3];
    f32 ez = mtx[2][0] * x + mtx[2][1] * y + mtx[2][2] * z + mtx[2][3];
    f32 cx, cy, cz, cw;
    if (pm[0] == 0.0f) {  // perspective
        cx = pm[1] * ex + pm[2] * ez; cy = pm[3] * ey + pm[4] * ez; cz = pm[5] * ez + pm[6]; cw = -ez;
    } else {
        cx = pm[1] * ex + pm[2]; cy = pm[3] * ey + pm[4]; cz = pm[5] * ez + pm[6]; cw = 1.0f;
    }
    if (cw == 0.0f) cw = 1e-6f;
    *sx_ = vp[2] / 2.0f * (cx / cw) + vp[0] + vp[2] / 2.0f;
    *sy_ = -vp[3] / 2.0f * (cy / cw) + vp[1] + vp[3] / 2.0f;
    *sz_ = (vp[5] - vp[4]) * (cz / cw) + vp[5];
}

void GXDrawTorus(f32 rc, u8 numc, u8 numt) {}
void* GXSetVerifyCallback(void* cb) { return NULL; }

}  // extern "C"
