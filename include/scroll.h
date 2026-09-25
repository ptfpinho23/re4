#ifndef SCROLL_H
#define SCROLL_H

#include "types.h"
#include "vec.h"
#include "obj.h"

// Scroll (room model) data file `SMD` (game/scroll.cpp). One SmdWork per placed model.
struct SmdWork {   // file-resident: big-endian fields
    BeVec pos;     // 0x00
    BeVec rot;     // 0x0C
    BeVec scale;   // 0x18
    u8 binNo;      // 0x24  bin table index (0xFF: none)
    u8 tplNo;      // 0x25  tpl table index (0xFF: none)
    u8 motNo;      // 0x26  motion table index (0xFF: none)
    u8 id;         // 0x27  scroll object id (0xFF: unused, 0xFE: not registered)
    u8 pad_28[0x44 - 0x28];
    union {
        be_u32 flags;   // 0x44  bit4: bin/tpl come from the common SMD, bit6: motion too
        struct {
            u8 pad_44[3];
            u8 attr;  // 0x47  low byte of flags -> cObj::attr (the file's last byte either way)
        } b;
    };
};

class cSmd {
public:
    u8 Version;    // 0x00
    u8 Flag;      // 0x01  bit0: group count table in front of the works
    be_u16 nModel;     // 0x02  (file-resident: big-endian fields)
    be_u32 BinTblOfs;  // 0x04  offset table of the bins
    be_u32 TplTblOfs;  // 0x08  offset table of the tpls
    be_u32 MotTblOfs;  // 0x0C  offset table of the motions
    union {
        SmdWork work[1];   // 0x10
        struct {
            be_u32 nGroup;    // 0x10
            be_u32 num[1];    // 0x14  works per group
        } grp;
    };

    void slide(int offset);
    SmdWork* getWorkPtr(int id);
    void* getBinPtr(int id);
    void* getTplPtr(int id);
    void* getMotPtr(int id);
    int getWorkNum();
};

// Scroll extra data `SMX`: per-id object parameters.
struct SmxWork {
    u8 id;         // 0x00
    u8 type;       // 0x01  -> cModel::type
    u8 type2;      // 0x02  -> cModel::x12F
    u8 CullMode;   // 0x03  -> cModel::CullMode
    be_u32 SelectMask;  // 0x04  -> cLightInfo::SelectMask  (file-resident: big-endian fields)
    be_u32 flags;     // 0x08  SmxSetFlag bits
    be_u32 color;     // 0x0C  -> cModelInfo::color
    u8 work[0x74]; // 0x10  copied to cObj::work (0x78 bytes including color2)
    be_u32 color2;    // 0x84
    be_f32 uvScrollU; // 0x88
    be_f32 uvScrollV; // 0x8C
};

class cSmx {
public:
    u8 x0;         // 0x00
    u8 nWork;      // 0x01
    u8 pad_2[0x10 - 0x02];
    SmxWork work[1];   // 0x10
};

// nScrWork is not declared here on purpose: the .sbss order of scroll.cpp follows the first
// declarations (pSmd, pSmdComn, pSmx, scrObjTbl, scrTbl, nScrWork).
extern cSmd* pSmd;
extern cSmd* pSmdComn;

int SmdInit(cSmd* pSh, cSmx* pSmxh, cSmd* pShCmn);
void SmdClear(int mode);
void workInit(cObj* pObj);
void SmdSetup(int blockNo);
int setObj(int blkNo);
int SmdSetParam(cObj* pObj, SmdWork* pSw);
void SmxSetFlag(cObj* pObj, u32 flag);
int SmxGetFlag(cObj* pObj);
void smxInit(cObj* obj, u8 id);
void smxInit(cObj* obj, SmxWork* w);
void* SmdGetTplPtr(int idx);
cObj* SmdGetObjPtr(u32 idx);
int SmdGetObjNum();
int SmdGetWorkId(cObj* pObj);
void BlockCreate(int blkNo, cSmd* pBlock);
void BlockDestroy(int blkNo);
SmdWork* SmdGetWorkPtr(int idx);
cObj* SmdGetGroupObjPtr(u32 idx);
cObj* SmdGetGroupObjPtr2(u32 idx);
cObj* SmdGetGroupNext(cObj* pObj00);
void SmdSetTrans(u32 idx, int onoff);
cObj* SetObjSmd(void* bin, void* tpl, Vec* pos, Vec* rot, int lightFlag, int front);

#endif
