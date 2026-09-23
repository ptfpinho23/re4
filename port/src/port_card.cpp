// port/src/port_card: the memory card API. No card is present: every probe reports
// CARD_RESULT_NOCARD, so the title screen takes its "no memory card" path (card.cpp errorSet(-3))
// instead of mounting a card that answers with zeros. A memory-stick save file is later work.
#include "types.h"
#include "card.h"
#define CARD_RESULT_NOCARD -3

extern "C" {

void CARDInit(void) {}
s32 CARDGetResultCode(s32 chan) { return CARD_RESULT_NOCARD; }
s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) { return CARD_RESULT_NOCARD; }
s32 CARDMountAsync(s32 chan, void* workArea, CardCallback detachCallback, CardCallback attachCallback) { return CARD_RESULT_NOCARD; }
s32 CARDUnmount(s32 chan) { return CARD_RESULT_NOCARD; }
s32 CARDCheckAsync(s32 chan, CardCallback callback) { return CARD_RESULT_NOCARD; }
s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed) { return CARD_RESULT_NOCARD; }
s32 CARDGetSerialNo(s32 chan, u64* serialNo) { return CARD_RESULT_NOCARD; }
s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CardFileInfo* fileInfo, CardCallback callback) { return CARD_RESULT_NOCARD; }
s32 CARDDeleteAsync(s32 chan, const char* fileName, CardCallback callback) { return CARD_RESULT_NOCARD; }
s32 CARDFormatAsync(s32 chan, CardCallback callback) { return CARD_RESULT_NOCARD; }
s32 CARDOpen(s32 chan, const char* fileName, CardFileInfo* fileInfo) { return CARD_RESULT_NOCARD; }
s32 CARDClose(CardFileInfo* fileInfo) { return CARD_RESULT_NOCARD; }
s32 CARDReadAsync(CardFileInfo* fileInfo, void* buf, s32 length, s32 offset, CardCallback callback) { return CARD_RESULT_NOCARD; }
s32 CARDWriteAsync(CardFileInfo* fileInfo, void* buf, s32 length, s32 offset, CardCallback callback) { return CARD_RESULT_NOCARD; }
s32 CARDGetStatus(s32 chan, s32 fileNo, CardStat* stat) { return CARD_RESULT_NOCARD; }
s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CardStat* stat, CardCallback callback) { return CARD_RESULT_NOCARD; }

}  // extern "C"
