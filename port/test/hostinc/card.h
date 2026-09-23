// Host test build: the memory card types and API of include/card.h without the game's cCard class
// (its operator new takes a 32-bit size).
#ifndef CARD_H
#define CARD_H
#include "types.h"
// Dolphin CARD SDK (the SDK header drags in the CodeWarrior libc).
struct CardFileInfo {
    s32 chan;    // 0x00
    s32 fileNo;  // 0x04
    s32 offset;  // 0x08
    s32 length;  // 0x0C
    u16 iBlock;  // 0x10
    u16 pad_12;
};

struct CardStat {
    char fileName[32];   // 0x00
    u32 length;          // 0x20
    u32 time;            // 0x24
    u8 gameName[4];      // 0x28
    u8 company[2];       // 0x2C
    u8 bannerFormat;     // 0x2E
    u8 pad_2F;
    u32 iconAddr;        // 0x30
    u16 iconFormat;      // 0x34
    u16 iconSpeed;       // 0x36
    u32 commentAddr;     // 0x38
    u32 offsetBanner;    // 0x3C
    u32 offsetBannerTlut;// 0x40
    u32 offsetIcon[8];   // 0x44
    u32 offsetIconTlut;  // 0x64
    u32 offsetData;      // 0x68
};                       // 0x6C

typedef void (*CardCallback)(s32 chan, s32 result);

extern "C" {
void CARDInit();
s32 CARDGetResultCode(s32 chan);
s32 CARDCheckAsync(s32 chan, CardCallback callback);
s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed);
s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CardFileInfo* fileInfo, CardCallback callback);
s32 CARDDeleteAsync(s32 chan, const char* fileName, CardCallback callback);
s32 CARDFormatAsync(s32 chan, CardCallback callback);
s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize);
s32 CARDMountAsync(s32 chan, void* workArea, CardCallback detachCallback, CardCallback attachCallback);
s32 CARDUnmount(s32 chan);
s32 CARDGetSerialNo(s32 chan, u64* serialNo);
s32 CARDOpen(s32 chan, const char* fileName, CardFileInfo* fileInfo);
s32 CARDClose(CardFileInfo* fileInfo);
s32 CARDReadAsync(CardFileInfo* fileInfo, void* buf, s32 length, s32 offset, CardCallback callback);
s32 CARDWriteAsync(CardFileInfo* fileInfo, void* buf, s32 length, s32 offset, CardCallback callback);
s32 CARDGetStatus(s32 chan, s32 fileNo, CardStat* stat);
s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CardStat* stat, CardCallback callback);
}

#endif
