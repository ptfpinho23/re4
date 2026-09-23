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
