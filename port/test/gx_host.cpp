// Host test build: drives the port's GX layer (port/src/port_gx.cpp) with a command script on
// stdin; the recorder (gx_fake_gu.cpp) prints what reaches the GE. tools/port/gxtest.py writes the
// scripts and checks the output.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "gx.h"

extern "C" void* GXInit(void* base, u32 size);
extern "C" void GXBeginDisplayList(void* list, u32 size);
extern "C" u32 GXEndDisplayList(void);
extern "C" void port_gx_put_u8(u8 v);
extern "C" void port_gx_put_u16(u16 v);
extern "C" void port_gx_put_s16(s16 v);
extern "C" void port_gx_put_f32(f32 v);
extern "C" void port_gx_put_s8(s8 v);

static u8* hexbuf(const char* hex, int* len)
{
    int n = (int) strlen(hex) / 2;
    u8* b = (u8*) malloc(n + 32);
    for (int i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        b[i] = (u8) v;
    }
    *len = n;
    return b;
}

int main()
{
    static GXTexObj tex;
    static GXTlutObj tluts[4];
    static u8* recorded;
    static u32 recordedLen;
    char line[1 << 20];
    GXInit(NULL, 0);
    while (fgets(line, sizeof(line), stdin)) {
        char cmd[32];
        if (sscanf(line, "%31s", cmd) != 1) continue;
        char* args = line + strlen(cmd);
        while (*args == ' ') args++;
        if (!strcmp(cmd, "vcd")) { int a, t; sscanf(args, "%d %d", &a, &t); GXSetVtxDesc(a, t); }
        else if (!strcmp(cmd, "clearvcd")) { GXClearVtxDesc(); }
        else if (!strcmp(cmd, "vat")) { int f, a, c, t, fr; sscanf(args, "%d %d %d %d %d", &f, &a, &c, &t, &fr); GXSetVtxAttrFmt(f, a, c, t, (u8) fr); }
        else if (!strcmp(cmd, "array")) { int a, s; char* hex = (char*) malloc(strlen(args) + 1); sscanf(args, "%d %d %s", &a, &s, hex); int n; u8* b = hexbuf(hex, &n); GXSetArray(a, b, (u8) s); }
        else if (!strcmp(cmd, "posmtx")) { int id; f32 m[12]; sscanf(args, "%d %f %f %f %f %f %f %f %f %f %f %f %f", &id, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7], &m[8], &m[9], &m[10], &m[11]); GXLoadPosMtxImm((const f32(*)[4]) m, (u32) id); }
        else if (!strcmp(cmd, "texmtx")) { int id; f32 m[8]; sscanf(args, "%d %f %f %f %f %f %f %f %f", &id, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7]); GXLoadTexMtxImm((const f32(*)[4]) m, (u32) id, 1); }
        else if (!strcmp(cmd, "curmtx")) { int id; sscanf(args, "%d", &id); GXSetCurrentMtx((u32) id); }
        else if (!strcmp(cmd, "texgen")) { int m; sscanf(args, "%d", &m); GXSetTexCoordGen2(0, 1, 4, (u32) m, 0, 125); }
        else if (!strcmp(cmd, "proj")) { int t; f32 m[16]; sscanf(args, "%d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &t, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7], &m[8], &m[9], &m[10], &m[11], &m[12], &m[13], &m[14], &m[15]); GXSetProjection((const f32(*)[4]) m, t); }
        else if (!strcmp(cmd, "viewport")) { f32 v[6]; sscanf(args, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]); GXSetViewport(v[0], v[1], v[2], v[3], v[4], v[5]); }
        else if (!strcmp(cmd, "dl")) { char* hex = (char*) malloc(strlen(args) + 1); sscanf(args, "%s", hex); int n; u8* b = hexbuf(hex, &n); GXCallDisplayList(b, (u32) n); }
        else if (!strcmp(cmd, "begin")) { int p, f, n; sscanf(args, "%d %d %d", &p, &f, &n); GXBegin(p, f, (u16) n); }
        else if (!strcmp(cmd, "put8")) { int v; sscanf(args, "%d", &v); port_gx_put_u8((u8) v); }
        else if (!strcmp(cmd, "puts8")) { int v; sscanf(args, "%d", &v); port_gx_put_s8((s8) v); }
        else if (!strcmp(cmd, "put16")) { int v; sscanf(args, "%d", &v); port_gx_put_u16((u16) v); }
        else if (!strcmp(cmd, "puts16")) { int v; sscanf(args, "%d", &v); port_gx_put_s16((s16) v); }
        else if (!strcmp(cmd, "putf")) { f32 v; sscanf(args, "%f", &v); port_gx_put_f32(v); }
        else if (!strcmp(cmd, "texobj")) { int f, w, h; char* hex = (char*) malloc(strlen(args) + 1); sscanf(args, "%d %d %d %s", &f, &w, &h, hex); int n; u8* b = hexbuf(hex, &n); GXInitTexObj(&tex, b, (u16) w, (u16) h, f, 1, 1, 0); GXLoadTexObj(&tex, 0); }
        else if (!strcmp(cmd, "texobjci")) { int f, w, h, tl; char* hex = (char*) malloc(strlen(args) + 1); sscanf(args, "%d %d %d %d %s", &f, &w, &h, &tl, hex); int n; u8* b = hexbuf(hex, &n); GXInitTexObjCI(&tex, b, (u16) w, (u16) h, f, 1, 1, 0, (u32) tl); GXLoadTexObj(&tex, 0); }
        else if (!strcmp(cmd, "tlut")) { int name, f, n; char* hex = (char*) malloc(strlen(args) + 1); sscanf(args, "%d %d %d %s", &name, &f, &n, hex); int len; u8* b = hexbuf(hex, &len); GXInitTlutObj(&tluts[name & 3], b, f, (u16) n); GXLoadTlut(&tluts[name & 3], (u32) name); }
        else if (!strcmp(cmd, "tevorder")) { int s, c, m, col; sscanf(args, "%d %d %d %d", &s, &c, &m, &col); GXSetTevOrder(s, c, m, col); }
        else if (!strcmp(cmd, "numtexgens")) { int n; sscanf(args, "%d", &n); GXSetNumTexGens((u8) n); }
        else if (!strcmp(cmd, "matcolor")) { unsigned r, g, b, a; sscanf(args, "%u %u %u %u", &r, &g, &b, &a); GXColor c = {(u8) r, (u8) g, (u8) b, (u8) a}; GXSetChanMatColor(0, c); }
        else if (!strcmp(cmd, "matsrc")) { int s; sscanf(args, "%d", &s); GXSetChanCtrl(0, 1, 0, s, 0, 0, 0); }
        else if (!strcmp(cmd, "invalidate")) { GXInvalidateTexAll(); }
        else if (!strcmp(cmd, "zmode")) { int e, f, u; sscanf(args, "%d %d %d", &e, &f, &u); GXSetZMode((u8) e, f, (u8) u); }
        else if (!strcmp(cmd, "blend")) { int t, s, d, o; sscanf(args, "%d %d %d %d", &t, &s, &d, &o); GXSetBlendMode(t, s, d, o); }
        else if (!strcmp(cmd, "cull")) { int m; sscanf(args, "%d", &m); GXSetCullMode(m); }
        else if (!strcmp(cmd, "alphacmp")) { int c0, r0, op, c1, r1; sscanf(args, "%d %d %d %d %d", &c0, &r0, &op, &c1, &r1); GXSetAlphaCompare(c0, (u8) r0, op, c1, (u8) r1); }
        else if (!strcmp(cmd, "colorupdate")) { int e; sscanf(args, "%d", &e); GXSetColorUpdate((u8) e); }
        else if (!strcmp(cmd, "alphaupdate")) { int e; sscanf(args, "%d", &e); GXSetAlphaUpdate((u8) e); }
        else if (!strcmp(cmd, "fog")) { int t; f32 a, b, c, d; unsigned r, g, bl, al; sscanf(args, "%d %f %f %f %f %u %u %u %u", &t, &a, &b, &c, &d, &r, &g, &bl, &al); GXColor col = {(u8) r, (u8) g, (u8) bl, (u8) al}; GXSetFog(t, a, b, c, d, col); }
        else if (!strcmp(cmd, "tevop")) { int st, m; sscanf(args, "%d %d", &st, &m); GXSetTevOp(st, m); }
        else if (!strcmp(cmd, "beginlist")) { int n; sscanf(args, "%d", &n); free(recorded); recorded = (u8*) malloc(n); GXBeginDisplayList(recorded, (u32) n); }
        else if (!strcmp(cmd, "endlist")) { recordedLen = GXEndDisplayList(); printf("RECORDED %u\n", recordedLen); }
        else if (!strcmp(cmd, "calllist")) { GXCallDisplayList(recorded, recordedLen); }
        else if (!strcmp(cmd, "echo")) { printf("%s", args); }
        else { fprintf(stderr, "unknown command %s\n", cmd); return 1; }
    }
    return 0;
}
