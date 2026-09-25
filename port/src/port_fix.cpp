// port/src/port_fix: load-time fix-ups of file data the game reads through structures it also uses
// at run time, so their fields cannot carry the big-endian types of port_be.h. AreaData
// (include/area.h) is the case: the trigger areas of the room's SAR (light areas) and BLK (block
// areas) files are byte-swapped in place once, where the loaders bind them (PORT_FIX in port.h).
#include "types.h"
#include "port.h"
#include "area.h"
#include "block.h"

static void swapArea(AreaData* a)
{
    // Be_flag / type are bytes; x2 and the eleven floats of the body are big-endian in the file.
    u16 x2;
    __builtin_memcpy(&x2, &a->x2, 2);
    x2 = (u16) __builtin_bswap16(x2);
    __builtin_memcpy(&a->x2, &x2, 2);
    u32* f = (u32*) &a->u;
    for (int i = 0; i < (int) (sizeof(AreaBody) / 4); i++) {
        f[i] = __builtin_bswap32(f[i]);
    }
}

extern "C" LightAreaHed* port_fix_light_area(LightAreaHed* p)
{
    // light_area.cpp: u32 num at 0, LightAreaData[num] at 0x10, each 0xD8 bytes with the area at +4
    if (p == NULL) return p;
    u8* b = (u8*) p;
    u32 n = __builtin_bswap32(*(u32*) b);
    for (u32 i = 0; i < n; i++) {
        swapArea((AreaData*) (b + 0x10 + i * 0xD8 + 4));
    }
    return p;
}

extern "C" BlockHeader* port_fix_block(BlockHeader* h)
{
    BlockArea* a = (BlockArea*) ((u8*) h + h->ofsArea);
    for (u32 i = 0; i < h->nArea; i++) {
        swapArea(&a[i].area);
    }
    return h;
}

#include "sce_at.h"
#include "flr_at.h"

extern "C" FlrAtHead* port_fix_flr(FlrAtHead* p)
{
    // flr_at.cpp: FlrAt records of 0x84 bytes from 0x10, each with an AreaData at +0x14 (the
    // payload's multi-byte fields are be_ typed)
    FlrAt* at = (FlrAt*) (p + 1);
    for (u32 i = 0; i < p->num; i++) {
        swapArea((AreaData*) at[i].area);
    }
    return p;
}

static inline void swap32(void* p)
{
    u32 v;
    __builtin_memcpy(&v, p, 4);
    v = __builtin_bswap32(v);
    __builtin_memcpy(p, &v, 4);
}
static inline void swap16(void* p)
{
    u16 v;
    __builtin_memcpy(&v, p, 2);
    v = (u16) __builtin_bswap16(v);
    __builtin_memcpy(p, &v, 2);
}
static inline void swapVec(Vec* v)
{
    swap32(&v->x);
    swap32(&v->y);
    swap32(&v->z);
}

// One AEV / ITA record (sce_at.h SceAtWork, 0x9C bytes): the fields the file carries, by area type.
// Pointer fields (next, func, pParent, the model / callback slots of the payloads) are filled at
// run time and left alone.
static void swapSceAt(SceAtWork* w)
{
    swapArea(&w->area);
    swap32(&w->arg);
    swap16(&w->parentParts);
    switch (w->type) {
    case SCEAT_ID_DOOR:
        swapVec(&w->dstPos);
        swap32(&w->dstAngle);
        swap32(&w->doorArg);
        break;
    case SCEAT_ID_ITEM:
    case SCEAT_ID_ITEM_PARENT:
        swapVec(&w->item.pos);
        swapVec(&w->item.ofs);
        swap16(&w->item.id);
        swap16(&w->item.flagNo);
        swap16(&w->item.num);
        swap16(&w->item.findFlagNo);
        swap16(&w->item.saveNo);
        swap32(&w->item.size);
        swapVec(&w->item.rot);
        break;
    case SCEAT_ID_FLG:
        swap16(&w->flg.no);
        break;
    case SCEAT_ID_MES:
        swap16(&w->mes.type);
        swap16(&w->mes.no);
        swap16(&w->mes.se);
        break;
    case SCEAT_ID_SAVE:
    case SCEAT_ID_FIELD_INFO:
        swap32(&w->value);
        break;
    case SCEAT_ID_SHD_DISP:
        swap16(&w->shd.no);
        break;
    case SCEAT_ID_DAMAGE:
        swap32(&w->dmg.time);
        swap32(&w->dmg.arg);
        swap32(&w->dmg.power);
        break;
    case SCEAT_ID_SCR_AT:
        swap32(&w->scr.attr);
        swap32(&w->scr.attr2);
        swap32(&w->scr.flags);
        swap32(&w->scr.flag);
        break;
    case SCEAT_ID_CAM_CTRL:
        swapVec(&w->cam.pos);
        swap32(&w->cam.angle);
        swap32(&w->cam.range);
        swap32(&w->cam.range2);
        break;
    case SCEAT_ID_LADDER:
        swapVec(&w->ladder.pos);
        swap32(&w->ladder.angle);
        break;
    case SCEAT_ID_USE:
        swap16(&w->useItem[0]);
        swap16(&w->useItem[1]);
        break;
    case SCEAT_ID_HIDE:
        swapVec(&w->hide.pos);
        break;
    case SCEAT_ID_POS_JUMP:
        swapVec(&w->jumpPos);
        break;
    default:
        break;
    }
}

extern "C" void* port_fix_sce_at(void* p)
{
    // sce_at.cpp SceAtFileHead: "AEV" / "ITA", u16 version, u16 num, the records from 0x10
    u8* b = (u8*) p;
    u16 num;
    __builtin_memcpy(&num, b + 6, 2);
    num = (u16) __builtin_bswap16(num);
    for (u32 i = 0; i < num; i++) {
        swapSceAt((SceAtWork*) (b + 0x10 + i * sizeof(SceAtWork)));
    }
    return p;
}

// A SAT file (atari.cpp cSat: a room's collision mesh): cSatFile header (u16 counts), then Vec
// vertices, Vec face normals, Vec edge normals, AtPoly polygons (0x14 bytes) and the block tree
// (cSatBlock: two Vec, four u16, the next-sibling offset, then u16 polygon indices or, with flag
// bit 0, an inline child block). The game keeps the file as its runtime structure (pointers into
// it for the collision math), so it is byte-swapped in place once. Runtime-built SATs
// (createBoxSat) never come through here.
#include "atari.h"
#include "at_sub.h"
static const void* satFixed[1024];
static int satFixedCount;

static inline void swap32p(void* p) { u32 v; __builtin_memcpy(&v, p, 4); v = __builtin_bswap32(v); __builtin_memcpy(p, &v, 4); }
static inline void swap16p(void* p) { u16 v; __builtin_memcpy(&v, p, 2); v = (u16) __builtin_bswap16(v); __builtin_memcpy(p, &v, 2); }
static inline u32 rd32(const void* p) { u32 v; __builtin_memcpy(&v, p, 4); return v; }
static inline u16 rd16(const void* p) { u16 v; __builtin_memcpy(&v, p, 2); return v; }

extern "C" void port_trace(const char* fmt, ...);
static int satBad;  // set when a block of the file being swapped is not credible: the walk stops

static void swapSatBlock(u8* b, u8* fileEnd, u32 nPoly, int depth)
{
    while (b && b + 0x24 <= fileEnd && !satBad) {
        for (int i = 0; i < 6; i++) swap32p(b + i * 4);      // min, size
        for (int i = 0; i < 4; i++) swap16p(b + 0x18 + i * 2);  // nFloor, nSlope, nWall, flag
        swap32p(b + 0x20);                                    // the next-sibling offset
        u16 flag = rd16(b + 0x1E);
        u32 next = rd32(b + 0x20);
        u32 n = rd16(b + 0x18) + rd16(b + 0x1A) + rd16(b + 0x1C);
        if (n > nPoly || (flag & ~3u) || next > 0x100000 || (next && next < 0x24) || depth > 24) {
            port_trace("[port] port_fix_sat: block %p not credible (n %u of %u, flag %x, next %x, depth %d)\n", b, n, nPoly, flag, next, depth);
            satBad = 1;
            return;
        }
        if (flag & 1) {
            swapSatBlock(b + 0x24, fileEnd, nPoly, depth + 1);  // one inline child (its chain follows it)
        } else {
            for (u32 i = 0; i < n; i++) swap16p(b + 0x24 + i * 2);
        }
        b = next ? b + next : NULL;
    }
}

static void satRegister(const void* f)
{
    if (satFixedCount < 1024) {
        satFixed[satFixedCount++] = f;
    } else {
        static int warned;
        if (!warned++) port_trace("[port] port_fix_sat: more than 1024 files: a file could be swapped twice\n");
    }
}

extern "C" void port_fix_sat_native(cSatFile* f)
{
    if (f) satRegister(f);
}

// A runtime-built or copied SAT is already in host order: its counts read sensibly as they are
// and not byte-swapped. (A file's counts read big-endian.)
static int satLooksNative(const u8* b)
{
    u32 np = rd16(b + 0xA), nv = rd16(b + 2);
    u32 npBE = (u16) __builtin_bswap16((u16) np), nvBE = (u16) __builtin_bswap16((u16) nv);
    return np != 0 && np <= 0x1FFF && nv != 0 && nv <= 0x4000 && (npBE > 0x1FFF || nvBE > 0x4000);
}

extern "C" cSatFile* port_fix_sat(cSatFile* f)
{
    if (f == NULL) return f;
    for (int i = 0; i < satFixedCount; i++) {
        if (satFixed[i] == f) return f;
    }
    u8* b = (u8*) f;
    if (satLooksNative(b)) {
        satRegister(f);
        return f;
    }
    if ((b[0] & 0x80) && b[0] != 0xFF) {  // a SAT table header (cSatHeader), not a file: the caller resolves it first
        port_trace("[port] port_fix_sat: %p is not a SAT file (version byte %02x)\n", f, b[0]);
        return f;
    }
    satRegister(f);
    for (int i = 1; i < 10; i++) swap16p(b + i * 2);          // the nine u16 counts after the version bytes
    u32 nv = rd16(b + 2), nn = rd16(b + 4), ne = rd16(b + 6), np = rd16(b + 0xA), nb = rd16(b + 0x12);
    if (nv > 0x4000 || nn > 0x4000 || ne > 0x4000 || np > 0x1FFF || nb > 0x1000) {
        port_trace("[port] port_fix_sat: %p counts not credible (%u vertices %u normals %u edges %u polygons %u blocks)\n", f, nv, nn, ne, np, nb);
        return f;
    }
    u8* p = b + 0x14;
    for (u32 i = 0; i < (nv + nn + ne) * 3; i++) swap32p(p + i * 4);
    p += (nv + nn + ne) * 12;
    for (u32 i = 0; i < np; i++) {  // AtPoly: seven u16 (vertices, normal, edges), a pad, the u32 attribute
        for (int k = 0; k < 7; k++) swap16p(p + i * 20 + k * 2);
        swap32p(p + i * 20 + 0x10);
    }
    p += np * 20;
    // the blocks: nb top-level blocks are not consecutive in general; the tree starts at the first
    u8* end = p + nb * 0x2000 + 0x10000;  // a generous bound: the chain itself ends the walk
    satBad = 0;
    if (nb) swapSatBlock(p, end, np, 0);
    return f;
}
