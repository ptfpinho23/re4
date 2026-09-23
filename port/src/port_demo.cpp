// port/src/port_demo: a scene the port draws when the game files are missing (port_dvd.cpp),
// through the same GX calls the game makes (immediate-mode vertices, GameCube-format textures,
// lights, projection): it shows the renderer on the PSP / PPSSPP without the discs. START exits.
#include "types.h"
#include "port.h"
#include "port_psp.h"
#include "gx.h"
#include "pad.h"
#include <math.h>
#include <string.h>

extern "C" void port_log(const char* fmt, ...);
extern "C" void VIWaitForRetrace(void);
extern "C" int pg_save_frame(const char* path);
extern "C" void pg_request_dump(const char* path);
extern "C" const char* port_data_root(void);
extern "C" int pg_stat_draws, pg_stat_verts, pg_stat_clears;

// GX enum values (dolphin/gx/GXEnum.h)
#define VA_POS 9
#define VA_NRM 10
#define VA_CLR0 11
#define VA_TEX0 13
#define T_DIRECT 1
#define CT_F32 4
#define CT_RGBA8 5
#define TF_I8 1
#define TF_RGB565 4
#define TF_RGB5A3 5
#define TF_RGBA8 6
#define TF_C8 9
#define TF_CMPR 14
#define TL_RGB5A3 2
#define PRIM_QUADS 0x80
#define PRIM_TRIANGLES 0x90

static u8 texI8[32 * 32] __attribute__((aligned(32)));
static u8 tex565[32 * 32 * 2] __attribute__((aligned(32)));
static u8 texRgba8[32 * 32 * 4] __attribute__((aligned(32)));
static u8 texCmpr[32 * 32 / 2] __attribute__((aligned(32)));
static u8 texC8[32 * 32] __attribute__((aligned(32)));
static u8 tlut[256 * 2] __attribute__((aligned(32)));
static GXTexObj texObj[5];
static GXTlutObj tlutObj;

static void be16(u8* p, u32 v) { p[0] = (u8) (v >> 8); p[1] = (u8) v; }

// The GameCube's tiled layouts, written the way the disc holds them.
static void makeTextures(void)
{
    // I8: 8x4 tiles, a checkerboard with a bright border
    for (int ty = 0; ty < 8; ty++) for (int tx = 0; tx < 4; tx++) for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) {
        int px = tx * 8 + x, py = ty * 4 + y;
        int c = ((px / 4) + (py / 4)) & 1 ? 0xE0 : 0x40;
        if (px == 0 || py == 0 || px == 31 || py == 31) c = 0xFF;
        texI8[(ty * 4 + tx) * 32 + y * 8 + x] = (u8) c;
    }
    // RGB565 and RGBA8: 4x4 tiles, a red-green gradient (alpha fades left to right in RGBA8)
    for (int ty = 0; ty < 8; ty++) for (int tx = 0; tx < 8; tx++) for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        int px = tx * 4 + x, py = ty * 4 + y;
        int i = (ty * 8 + tx) * 16 + y * 4 + x;
        u32 r = px * 31 / 31, g = py * 63 / 31;
        be16(tex565 + i * 2, (r << 11) | (g << 5) | 8);
        int t = (ty * 8 + tx) * 64;  // tile: 32 bytes A,R pairs then 32 bytes G,B pairs
        int k = y * 4 + x;
        texRgba8[t + k * 2] = (u8) (255 - px * 8);
        texRgba8[t + k * 2 + 1] = (u8) (px * 8);
        texRgba8[t + 32 + k * 2] = (u8) (py * 8);
        texRgba8[t + 32 + k * 2 + 1] = (u8) 200;
    }
    // CMPR: 8x8 tiles of four 4x4 DXT1 blocks; each block two colours, a diagonal index pattern
    for (int ty = 0; ty < 4; ty++) for (int tx = 0; tx < 4; tx++) for (int sub = 0; sub < 4; sub++) {
        u8* b = texCmpr + ((ty * 4 + tx) * 4 + sub) * 8;
        u32 c0 = (u32) ((31 - tx * 8) << 11) | (u32) ((ty * 16) << 5) | 31;   // blue-ish
        u32 c1 = (u32) (31 << 11) | (u32) ((63 - ty * 16) << 5) | 0;         // yellow-ish
        if (c0 <= c1) { u32 t = c0; c0 = c1 + 1; c1 = t; }
        be16(b, c0); be16(b + 2, c1);
        for (int row = 0; row < 4; row++) {
            u32 idx = 0;
            for (int col = 0; col < 4; col++) idx |= (u32) (((row + col + sub) >> 1) & 3) << (6 - col * 2);
            b[4 + row] = (u8) idx;
        }
    }
    // C8 with an RGB5A3 palette: 8x4 tiles, rings of palette index by distance from the centre
    for (int i = 0; i < 256; i++) {
        u32 r = (i * 7) & 31, g = (i * 3) & 31, bl = (31 - (i & 31));
        be16(tlut + i * 2, 0x8000 | (r << 10) | (g << 5) | bl);
    }
    for (int ty = 0; ty < 8; ty++) for (int tx = 0; tx < 4; tx++) for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) {
        int px = tx * 8 + x, py = ty * 4 + y;
        int d = (px - 16) * (px - 16) + (py - 16) * (py - 16);
        texC8[(ty * 4 + tx) * 32 + y * 8 + x] = (u8) (d / 2);
    }
    GXInitTexObj(&texObj[0], texI8, 32, 32, TF_I8, 1, 1, 0);
    GXInitTexObj(&texObj[1], tex565, 32, 32, TF_RGB565, 1, 1, 0);
    GXInitTexObj(&texObj[2], texRgba8, 32, 32, TF_RGBA8, 0, 0, 0);
    GXInitTexObj(&texObj[3], texCmpr, 32, 32, TF_CMPR, 1, 1, 0);
    GXInitTlutObj(&tlutObj, tlut, TL_RGB5A3, 256);
    GXLoadTlut(&tlutObj, 0);
    GXInitTexObjCI(&texObj[4], texC8, 32, 32, TF_C8, 0, 0, 0, 0);
}

static void identity34(f32 m[3][4])
{
    memset(m, 0, 12 * sizeof(f32));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

// C_MTXPerspective / C_MTXOrtho as the SDK defines them.
static void perspective(f32 m[4][4], f32 fovy, f32 aspect, f32 n, f32 f)
{
    memset(m, 0, 16 * sizeof(f32));
    f32 cot = 1.0f / tanf(fovy * 0.5f * 3.14159265f / 180.0f);
    f32 tmp = 1.0f / (f - n);
    m[0][0] = cot / aspect;
    m[1][1] = cot;
    m[2][2] = -n * tmp;
    m[2][3] = -(f * n) * tmp;
    m[3][2] = -1.0f;
}
static void ortho(f32 m[4][4], f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
    memset(m, 0, 16 * sizeof(f32));
    f32 tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp; m[0][3] = -(r + l) * tmp;
    tmp = 1.0f / (t - b);
    m[1][1] = 2.0f * tmp; m[1][3] = -(t + b) * tmp;
    tmp = 1.0f / (f - n);
    m[2][2] = -1.0f * tmp; m[2][3] = -f * tmp;
    m[3][3] = 1.0f;
}

static void vertexFormat(int withNormal)
{
    GXClearVtxDesc();
    GXSetVtxDesc(VA_POS, T_DIRECT);
    if (withNormal) GXSetVtxDesc(VA_NRM, T_DIRECT);
    GXSetVtxDesc(VA_CLR0, T_DIRECT);
    GXSetVtxDesc(VA_TEX0, T_DIRECT);
    GXSetVtxAttrFmt(0, VA_POS, 1, CT_F32, 0);
    GXSetVtxAttrFmt(0, VA_NRM, 0, CT_F32, 0);
    GXSetVtxAttrFmt(0, VA_CLR0, 1, CT_RGBA8, 0);
    GXSetVtxAttrFmt(0, VA_TEX0, 1, CT_F32, 0);
}

static f32 quadZ = 0.0f;
static void quad2d(f32 x, f32 y, f32 w, f32 h, u32 rgba)
{
    GXBegin(PRIM_QUADS, 0, 4);
    GXPosition3f32(x, y, quadZ);     GXColor4u8((u8) (rgba >> 24), (u8) (rgba >> 16), (u8) (rgba >> 8), (u8) rgba); GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(x + w, y, quadZ); GXColor4u8((u8) (rgba >> 24), (u8) (rgba >> 16), (u8) (rgba >> 8), (u8) rgba); GXTexCoord2f32(1.0f, 0.0f);
    GXPosition3f32(x + w, y + h, quadZ); GXColor4u8((u8) (rgba >> 24), (u8) (rgba >> 16), (u8) (rgba >> 8), (u8) rgba); GXTexCoord2f32(1.0f, 1.0f);
    GXPosition3f32(x, y + h, quadZ); GXColor4u8((u8) (rgba >> 24), (u8) (rgba >> 16), (u8) (rgba >> 8), (u8) rgba); GXTexCoord2f32(0.0f, 1.0f);
}

static void cubeFace(const f32* p0, const f32* p1, const f32* p2, const f32* p3, const f32* n)
{
    const f32* p[4] = {p0, p1, p2, p3};
    static const f32 st[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; i++) {
        GXPosition3f32(p[i][0], p[i][1], p[i][2]);
        GXNormal3f32(n[0], n[1], n[2]);
        GXColor4u8(255, 255, 255, 255);
        GXTexCoord2f32(st[i][0], st[i][1]);
    }
}

static void drawCube(void)
{
    static const f32 v[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    static const f32 nz[3] = {0, 0, -1}, pz[3] = {0, 0, 1}, nx[3] = {-1, 0, 0}, px[3] = {1, 0, 0}, ny[3] = {0, -1, 0}, py[3] = {0, 1, 0};
    GXBegin(PRIM_QUADS, 0, 24);
    cubeFace(v[0], v[3], v[2], v[1], nz);
    cubeFace(v[5], v[6], v[7], v[4], pz);
    cubeFace(v[4], v[7], v[3], v[0], nx);
    cubeFace(v[1], v[2], v[6], v[5], px);
    cubeFace(v[4], v[0], v[1], v[5], ny);
    cubeFace(v[3], v[7], v[6], v[2], py);
}

static void rotationMatrix(f32 m[3][4], f32 ax, f32 ay, f32 tz)
{
    f32 cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay);
    // Ry * Rx, then a translation down the view axis
    m[0][0] = cy;        m[0][1] = sy * sx;   m[0][2] = sy * cx;   m[0][3] = 0.0f;
    m[1][0] = 0.0f;      m[1][1] = cx;        m[1][2] = -sx;       m[1][3] = 0.0f;
    m[2][0] = -sy;       m[2][1] = cy * sx;   m[2][2] = cy * cx;   m[2][3] = tz;
}

// One second per experiment, the frame saved at its end: 0 an untextured quad, 1 a textured quad,
// 2 with blending, 3 with the depth test, 4 the unlit cube, 5 the lit cube, 6 the whole scene.
extern "C" void port_demo_run(void)
{
    port_log("[port] renderer demo: GameCube textures (I8 565 RGBA8 CMPR C8), a lit cube. START exits.\n");
    makeTextures();
    GXColor clear = {24, 24, 48, 255};
    GXSetCopyClear(clear, 0xFFFFFF);
    GXSetViewport(0, 0, 640, 448, 0, 1);
    GXSetScissor(0, 0, 640, 448);
    GXSetCullMode(0);
    GXSetNumChans(1);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(0, 1, 4, 60);
    GXSetTevOrder(0, 0, 0, 4);
    GXSetTevOp(0, 0);
    GXSetAlphaCompare(7, 0, 0, 7, 0);
    GXLightObj light;
    GXColor white = {255, 255, 255, 255};
    GXColor amb = {40, 40, 70, 255};
    f32 t = 0.0f;
    int frame = 0;
    for (;;) {
        PADStatus pad[4];  // PADRead fills the four channels
        PADRead(pad);
        if (pad[0].button & 0x1000) break;
        int mode = frame / 60;
        if (mode > 6) mode = 6;
        GXSetBlendMode(mode == 2 || mode == 6 ? 1 : 0, 4, 5, 0);
        GXSetZMode(0, 3, 0);
        f32 o[4][4];
        ortho(o, 0.0f, 448.0f, 0.0f, 640.0f, 0.0f, 1.0f);
        GXSetProjection(o, 1);
        f32 id[3][4];
        identity34(id);
        GXLoadPosMtxImm(id, 0);
        GXSetCurrentMtx(0);
        GXSetChanCtrl(0, 0, 0, 1, 0, 0, 2);
        vertexFormat(0);
        if (mode <= 1) {
            GXSetNumTexGens(0);          // untextured: the vertex colour alone; mode 1 inside the depth range
            quadZ = mode == 0 ? 0.0f : -0.5f;
            quad2d(100.0f, 100.0f, 300.0f, 200.0f, 0xFF4040FF);
        } else {
            GXSetNumTexGens(1);
            if (mode < 4) {
                GXLoadTexObj(&texObj[1], 0);
                quad2d(100.0f, 100.0f, 300.0f, 200.0f, 0xFFFFFFFF);
            }
            if (mode == 6) {
                for (int i = 0; i < 5; i++) {
                    GXLoadTexObj(&texObj[i], 0);
                    quad2d(30.0f + i * 120.0f, 40.0f, 96.0f, 96.0f, 0xFFFFFFFF);
                }
            }
        }
        if (mode >= 3) GXSetZMode(1, 3, 1);
        if (mode >= 4) {
            f32 p[4][4];
            perspective(p, 45.0f, 640.0f / 448.0f, 0.5f, 100.0f);
            GXSetProjection(p, 0);
            f32 mv[3][4];
            rotationMatrix(mv, t * 0.7f, t, -5.0f);
            mv[1][3] = -0.6f;
            GXLoadPosMtxImm(mv, 0);
            GXLoadNrmMtxImm(mv, 0);
            GXSetCurrentMtx(0);
            if (mode >= 5) {
                GXInitLightColor(&light, white);
                GXInitLightPos(&light, 3.0f * cosf(t * 1.3f), 2.0f, -2.0f + 2.0f * sinf(t * 1.3f));
                GXInitLightDir(&light, 0.0f, 0.0f, 1.0f);
                GXInitLightAttn(&light, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
                GXLoadLightObjImm(&light, 1);
                GXSetChanAmbColor(4, amb);
                GXSetChanMatColor(4, white);
                GXSetChanCtrl(0, 1, 0, 0, 1, 2, 1);
                GXSetChanCtrl(2, 0, 0, 0, 0, 2, 2);
            }
            vertexFormat(1);
            GXLoadTexObj(&texObj[mode == 6 ? (frame / 180) % 5 : 0], 0);
            drawCube();
        }
        if (frame % 60 == 59 && frame < 7 * 60) {
            char path[128];
            const char* root = port_data_root();
            strcpy(path, root && root[0] ? root : "ms0:/");
            strcat(path, "../demo0.raw");
            path[strlen(path) - 5] = (char) ('0' + mode);
            pg_request_dump(path);
            port_log("[port] frame %d: %s; %d draws %d verts %d clears\n", frame, path, pg_stat_draws, pg_stat_verts, pg_stat_clears);
        }
        GXCopyDisp(NULL, 1);
        VIWaitForRetrace();
        t += 0.02f;
        frame++;
        pg_stat_draws = pg_stat_verts = pg_stat_clears = 0;
    }
    sceKernelExitGame();
}
