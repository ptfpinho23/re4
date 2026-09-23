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
#include "gx.h"
#include <string.h>
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
static const TlutPort* tlutTable[20];

struct TevStage {
    u8 coord, map, color, op;
};
static TevStage tev[16];
static u8 numTevStages = 1, numTexGens = 1, numChans = 1;
static u32 texGenMtx[8];         // GX_IDENTITY or a texture matrix id
static u32 chanMatColor[2] = {0xFFFFFFFF, 0xFFFFFFFF};
static u32 tevRegColor[4];

static int cullMode;
static u32 copyClearColor = 0xFF000000;
static u32 copyClearZ = 0xFFFFFF;
static u16 copySrc[4];           // GXSetTexCopySrc: left, top, width, height (EFB pixels)
static u16 copyDstW, copyDstH;
static u8 copyDstFmt;
// Framebuffer copies (GXCopyTex): the game's destination buffer stays untouched and is the key; the
// frame is read back into an 8888 buffer of the copy's size that the texture cache hands out.
struct CopyRecord {
    const void* dest;
    u16 w, h;
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
static int overlayNext;
static int overlayCount;

extern "C" void port_gx_overlay_line(const char* text)
{
    strncpy(overlay[overlayNext], text, OVERLAY_COLS - 1);
    overlay[overlayNext][OVERLAY_COLS - 1] = 0;
    char* nl = strchr(overlay[overlayNext], '\n');
    if (nl) *nl = 0;
    overlayNext = (overlayNext + 1) % OVERLAY_LINES;
    if (overlayCount < OVERLAY_LINES) overlayCount++;
}

extern "C" int port_gx_active(void) { return inited; }

// Drawn straight into the displayed frame after the swap (the debug screen's pixel writer).
static void drawOverlay(void)
{
    int first = (overlayNext - overlayCount + OVERLAY_LINES) % OVERLAY_LINES;
    pg_overlay_begin();
    for (int i = 0; i < overlayCount; i++) {
        pg_debug_print(0, i, 0xFF40FF40, overlay[(first + i) % OVERLAY_LINES]);
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
    u16 w, h;
    u8 fmt;
    u8 psm;
    void* converted;
    u32 lastUse;
};
#define TEXCACHE_MAX 384
static TexCacheEntry texCache[TEXCACHE_MAX];
static u32 texUseClock;

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

// Decodes a GameCube texture into 32-bit ABGR (the GE's 8888) or, for CMPR, into GE DXT1.
static void* convertTexture(const TexObjPort* t, const TlutPort* tlut, u8* psmOut)
{
    int w = t->width, h = t->height, fmt = t->format;
    const u8* src = (const u8*) t->data;
    if (fmt == 14) {  // CMPR: 8x8 tiles of four 4x4 DXT1 blocks -> GE DXT1 (indices then colours)
        *psmOut = PG_PSM_DXT1;
        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        u8* out = (u8*) ALIGNED_ALLOC(bw * bh * 8);
        if (!out) return NULL;
        for (int ty = 0; ty < (h + 7) / 8; ty++) {
            for (int tx = 0; tx < (w + 7) / 8; tx++) {
                for (int sub = 0; sub < 4; sub++) {
                    int bx = tx * 2 + (sub & 1), by = ty * 2 + (sub >> 1);
                    if (bx >= bw || by >= bh) { src += 8; continue; }
                    u8* d = out + (by * bw + bx) * 8;
                    for (int i = 0; i < 4; i++) {  // index rows: GC keeps texel 0 in the top bits
                        u8 b = src[4 + i];
                        d[i] = (u8) (((b & 0x03) << 6) | ((b & 0x0C) << 2) | ((b & 0x30) >> 2) | ((b & 0xC0) >> 6));
                    }
                    d[4] = src[1]; d[5] = src[0];  // colour 0, little-endian
                    d[6] = src[3]; d[7] = src[2];  // colour 1
                    src += 8;
                }
            }
        }
        return out;
    }
    *psmOut = PG_PSM_8888;
    u32* out = (u32*) ALIGNED_ALLOC(w * h * 4);
    if (!out) return NULL;
    int tw = 4, th = 4;  // tile size
    switch (fmt) {
    case 0: case 8: tw = 8; th = 8; break;              // I4, C4
    case 1: case 2: case 9: tw = 8; th = 4; break;      // I8, IA4, C8
    default: break;                                     // IA8, RGB565, RGB5A3, RGBA8, C14X2
    }
    const u8* lut = tlut ? (const u8*) tlut->lut : NULL;
    int lutFmt = tlut ? tlut->fmt : 2;
    for (int ty = 0; ty < (h + th - 1) / th; ty++) {
        for (int tx = 0; tx < (w + tw - 1) / tw; tx++) {
            for (int y = 0; y < th; y++) {
                for (int x = 0; x < tw; x++) {
                    int px = tx * tw + x, py = ty * th + y;
                    int i = y * tw + x;
                    u32 c = 0;
                    switch (fmt) {
                    case 0: { u8 v = src[i >> 1]; v = (i & 1) ? (v & 15) : (v >> 4); v *= 17; c = 0xFF000000u | v << 16 | v << 8 | v; break; }
                    case 1: { u8 v = src[i]; c = 0xFF000000u | v << 16 | v << 8 | v; break; }
                    case 2: { u8 v = src[i]; u8 a = (v >> 4) * 17, l = (v & 15) * 17; c = (u32) a << 24 | l << 16 | l << 8 | l; break; }
                    case 3: { u8 a = src[i * 2], l = src[i * 2 + 1]; c = (u32) a << 24 | l << 16 | l << 8 | l; break; }
                    case 4: c = tlutColor(src, 1, i); break;
                    case 5: c = tlutColor(src, 2, i); break;
                    case 6: { u8 a = src[i * 2], r = src[i * 2 + 1], g = src[32 + i * 2], b = src[32 + i * 2 + 1]; c = (u32) a << 24 | b << 16 | g << 8 | r; break; }
                    case 8: { u8 v = src[i >> 1]; v = (i & 1) ? (v & 15) : (v >> 4); c = lut ? tlutColor(lut, lutFmt, v) : 0xFF000000u; break; }
                    case 9: c = lut ? tlutColor(lut, lutFmt, src[i]) : 0xFF000000u; break;
                    case 10: c = lut ? tlutColor(lut, lutFmt, be16(src + i * 2) & 0x3FFF) : 0xFF000000u; break;
                    default: c = 0xFFFF00FF; break;
                    }
                    if (px < w && py < h) out[py * w + px] = c;
                }
            }
            src += (fmt == 0 || fmt == 8) ? 32 : (fmt == 1 || fmt == 2 || fmt == 9) ? 32 : (fmt == 6) ? 64 : 32;
        }
    }
    return out;
}

static TexCacheEntry* cachedTexture(const TexObjPort* t)
{
    for (int i = 0; i < COPY_MAX; i++) {
        if (copies[i].buf && copies[i].dest == t->data) {
            static TexCacheEntry copyEntry;
            copyEntry.data = t->data; copyEntry.tlut = NULL;
            copyEntry.w = copies[i].w; copyEntry.h = copies[i].h;
            copyEntry.fmt = t->format; copyEntry.psm = PG_PSM_8888;
            copyEntry.converted = copies[i].buf;
            return &copyEntry;
        }
    }
    const TlutPort* tlut = (t->isCI && t->tlutName < 20) ? tlutTable[t->tlutName] : NULL;
    const void* lutPtr = tlut ? tlut->lut : NULL;
    TexCacheEntry* victim = NULL;
    for (int i = 0; i < TEXCACHE_MAX; i++) {
        TexCacheEntry* e = &texCache[i];
        if (e->converted && e->data == t->data && e->tlut == lutPtr && e->w == t->width && e->h == t->height && e->fmt == t->format) {
            e->lastUse = ++texUseClock;
            return e;
        }
        if (!e->converted) {
            if (!victim) victim = e;
        } else if (!victim || (victim->converted && e->lastUse < victim->lastUse)) {
            if (!victim || victim->converted) victim = e;
        }
    }
    if (!victim) victim = &texCache[0];
    if (victim->converted) {
        free(victim->converted);
        victim->converted = NULL;
    }
    u8 psm = PG_PSM_8888;
    void* conv = convertTexture(t, tlut, &psm);
    if (!conv) return NULL;
    pg_dcache_writeback(conv, psm == PG_PSM_DXT1 ? ((t->width + 3) / 4) * ((t->height + 3) / 4) * 8 : t->width * t->height * 4);
    victim->data = t->data;
    victim->tlut = lutPtr;
    victim->w = t->width;
    victim->h = t->height;
    victim->fmt = t->format;
    victim->psm = psm;
    victim->converted = conv;
    victim->lastUse = ++texUseClock;
    return victim;
}

static void invalidateTextures(void)
{
    for (int i = 0; i < TEXCACHE_MAX; i++) {
        if (texCache[i].converted) {
            free(texCache[i].converted);
            texCache[i].converted = NULL;
        }
    }
}

// ---------------------------------------------------------------- per-draw state
static int texturedDraw(void)
{
    if (numTexGens == 0 || tev[0].map == 0xFF || tev[0].map >= 8 || texMap[tev[0].map] == NULL) {
        return 0;
    }
    return 1;
}

static void applyDrawState(int hasVertexColor)
{
    int textured = texturedDraw();
    if (textured) {
        TexObjPort* t = texMap[tev[0].map];
        TexCacheEntry* e = cachedTexture(t);
        if (e) {
            int tfx = PG_TFX_MODULATE;
            if (tev[0].op == 3) tfx = PG_TFX_REPLACE;       // GX_REPLACE
            else if (tev[0].op == 1) tfx = PG_TFX_DECAL;    // GX_DECAL
            pg_texture(e->psm, e->w, e->h, e->converted, t->wrapS == 0 ? 1 : 0, t->wrapT == 0 ? 1 : 0,
                       t->minFilt == 0 ? 0 : 1, t->magFilt == 0 ? 0 : 1, tfx);
        } else {
            pg_texture_off();
        }
    } else {
        pg_texture_off();
    }
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

static void applyTexMtx(u32 id, f32* s, f32* t)
{
    if (id == 60 || id >= 64) return;  // GX_IDENTITY
    const f32* m = &mtxMem[id][0];
    f32 ns = m[0] * *s + m[1] * *t + m[3];
    f32 nt = m[4] * *s + m[5] * *t + m[7];
    *s = ns;
    *t = nt;
}

// Parses `count` vertices of vertex format `vf` from the stream and draws them as `prim`.
static const u8* drawVertices(Parser* s, int prim, int vf, u32 count)
{
    if (vf > 7 || count == 0) return s->p;
    const Vat* f = &vat[vf];
    int quads = prim == 0;
    u32 outCount = quads ? count / 4 * 6 : count;
    PgVertex* out = (PgVertex*) pg_get_memory(outCount * sizeof(PgVertex));
    if (!out) return s->p;
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
        for (int a = 0; a < A_COUNT; a++) {
            u8 t = vcd[a];
            if (t == 0) continue;
            if (a == A_PNMTX) {
                u32 idx = *s->p++;
                if (idx < 64) { m = &mtxMem[idx][0]; nm = &nrmMem[idx][0]; }
                continue;
            }
            if (a >= A_TEXMTX0 && a < A_POS) {
                s->p++;
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
                if (lit) {  // s8 normals are 1.0 = 64, s16 1.0 = 16384 (GX fixes the fraction)
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
            v.color = lightColor(ch, v.color, tn, v.x, v.y, v.z);
        } else if (ch >= 0 && chan[ch].enable) {
            // lit without normals: ambient only
            static const f32 up[3] = {0.0f, 0.0f, 0.0f};
            v.color = lightColor(ch, v.color, up, v.x, v.y, v.z);
        }
        applyTexMtx(texGenMtx[0], &v.u, &v.v);
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
    if (cullMode == 3) return s->p;  // GX_CULL_ALL
    applyDrawState(hasColor);
    static const int primTbl[8] = {PG_TRIANGLES, PG_TRIANGLES, PG_TRIANGLES, PG_TRIANGLE_STRIP, PG_TRIANGLE_FAN, PG_LINES, PG_LINE_STRIP, PG_POINTS};
    pg_draw(primTbl[prim & 7], (int) o, out);
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
    for (int i = 0; i < 64; i++) {
        nrmMem[i][0] = (i % 3 == 0) ? 1.0f : 0.0f;
        nrmMem[i][1] = (i % 3 == 1) ? 1.0f : 0.0f;
        nrmMem[i][2] = (i % 3 == 2) ? 1.0f : 0.0f;
    }
    for (int i = 0; i < 8; i++) texGenMtx[i] = 60;
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

void GXCopyDisp(void* dest, u8 clear)
{
    if (!inited) GXInit_port();
    immFlush();
    pg_finish();
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
    immFlush();
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

void GXSetBlendMode(int type, int src_factor, int dst_factor, int op)
{
    if (!inited) return;
    static const int srcTbl[8] = {PG_FIX, PG_FIX, PG_SRC_COLOR, PG_ONE_MINUS_SRC_COLOR, PG_SRC_ALPHA, PG_ONE_MINUS_SRC_ALPHA, PG_DST_ALPHA, PG_ONE_MINUS_DST_ALPHA};
    static const int dstTbl[8] = {PG_FIX, PG_FIX, PG_DST_COLOR, PG_ONE_MINUS_DST_COLOR, PG_SRC_ALPHA, PG_ONE_MINUS_SRC_ALPHA, PG_DST_ALPHA, PG_ONE_MINUS_DST_ALPHA};
    u32 fixS = src_factor == 1 ? 0xFFFFFF : 0;
    u32 fixD = dst_factor == 1 ? 0xFFFFFF : 0;
    if (type == 1) {        // GX_BM_BLEND
        pg_blend(1, PG_ADD, srcTbl[src_factor & 7], dstTbl[dst_factor & 7], fixS, fixD);
    } else if (type == 3) { // GX_BM_SUBTRACT: dst - src
        pg_blend(1, PG_REVERSE_SUBTRACT, PG_FIX, PG_FIX, 0xFFFFFF, 0xFFFFFF);
    } else {
        pg_blend(0, 0, 0, 0, 0, 0);
    }
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

void GXSetTevOp(int stage, int mode)
{
    if (stage >= 0 && stage < 16) tev[stage].op = (u8) mode;
}

void GXSetTevColorIn(int stage, int a, int b, int c, int d) {}
void GXSetTevAlphaIn(int stage, int a, int b, int c, int d) {}
void GXSetTevColorOp(int stage, int op, int bias, int scale, u8 clamp, int out_reg) {}
void GXSetTevAlphaOp(int stage, int op, int bias, int scale, u8 clamp, int out_reg) {}
void GXSetTevColor(int id, GXColor color)
{
    if (id >= 0 && id < 4) tevRegColor[id] = ((u32) color.a << 24) | ((u32) color.b << 16) | ((u32) color.g << 8) | color.r;
}
void GXSetTevColorS10(int id, GXColorS10 color) {}
void GXSetTevKColor(int id, GXColor color) {}
void GXSetTevKColorSel(int stage, int sel) {}
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
    if (dst >= 0 && dst < 8) texGenMtx[dst] = mtx;
}

void GXSetChanCtrl(int ch, u8 enable, int amb_src, int mat_src, u32 light_mask, int diff_fn, int attn_fn)
{
    if (ch < 0 || ch > 3) return;
    Chan* c = &chan[ch];
    c->enable = enable; c->ambSrc = (u8) amb_src; c->matSrc = (u8) mat_src;
    c->lightMask = light_mask; c->diffFn = (u8) diff_fn; c->attnFn = (u8) attn_fn;
}
// GX_COLOR0 / GX_ALPHA0 / GX_COLOR0A0 (0, 2, 4) set pair 0; GX_COLOR1 / GX_ALPHA1 / GX_COLOR1A1 pair 1.
void GXSetChanMatColor(int ch, GXColor color)
{
    u32 c = ((u32) color.a << 24) | ((u32) color.b << 16) | ((u32) color.g << 8) | color.r;
    if (ch == 0 || ch == 2 || ch == 4) chanMatColor[0] = c; else chanMatColor[1] = c;
}
void GXSetChanAmbColor(int ch, GXColor color)
{
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
    if (tlut_name < 20) tlutTable[tlut_name] = (const TlutPort*) obj;
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
        r->buf = (u32*) ALIGNED_ALLOC(copyDstW * copyDstH * 4);
        if (!r->buf) return;
        r->dest = dest; r->w = copyDstW; r->h = copyDstH;
    }
    // GX_CTF_A8 (0x27) keeps the alpha, the Z formats (GX_TF_Z8 0x11 .. GX_TF_Z24X8 0x16) the depth
    int mode = copyDstFmt == 0x27 ? 1 : (copyDstFmt >= 0x11 && copyDstFmt <= 0x16) ? 2 : 0;
    pg_copy_frame(r->buf, r->w, r->h, (int) (copySrc[0] * sx()), (int) (copySrc[1] * sy()),
                  (int) (copySrc[2] * sx()), (int) (copySrc[3] * sy()), mode);
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
