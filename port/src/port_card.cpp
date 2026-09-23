// port/src/port_card: the memory card API (include/card.h, the Dolphin CARD library) over files
// on the memory stick. One card in slot A, 16 Mbit with 8 KB sectors (the size the game checks),
// its files under <root>/card/ next to the data tree: "<name>" holds the file's data, "<name>.stat"
// its CardStat (icon / comment / banner offsets the game fills in). Every asynchronous call
// completes at once and CARDGetResultCode returns its result. The saves are the game's own
// structures as this build lays them out, so they are not GameCube saves.
#include "types.h"
#include "port.h"
#include "port_psp.h"
#include "card.h"
#include <string.h>
#include <stdio.h>

extern "C" const char* port_data_root(void);
#ifdef __PSP__
extern "C" void port_log(const char* fmt, ...);
#endif

#define CARD_RESULT_READY 0
#define CARD_RESULT_BUSY -1
#define CARD_RESULT_NOCARD -3
#define CARD_RESULT_NOFILE -4
#define CARD_RESULT_IOERROR -5
#define CARD_RESULT_EXIST -7
#define CARD_RESULT_NOENT -8
#define CARD_RESULT_INSSPACE -9
#define CARD_RESULT_FATAL_ERROR -128

#define SECTOR 0x2000
#define CARD_BLOCKS 251          // a 16 Mbit card
#define MAX_FILES 16

static s32 lastResult;
static char cardDir[128];
static int cardReady;            // the directory exists
static struct {
    char name[32];
    u32 length;
    int used;
} files[MAX_FILES];

static int cardInit(void)
{
    if (cardDir[0]) return cardReady;
    const char* root = port_data_root();
    if (!root || !root[0]) {
        cardDir[0] = '?';
        return 0;
    }
    // "<root>/data/" -> "<root>/card/"
    strncpy(cardDir, root, sizeof(cardDir) - 8);
    size_t n = strlen(cardDir);
    if (n >= 5 && strcmp(cardDir + n - 5, "data/") == 0) cardDir[n - 5] = 0;
    strcat(cardDir, "card/");
    char probe[160];
    strcpy(probe, cardDir);
    probe[strlen(probe) - 1] = 0;
    SceUID_ d = sceIoDopen(probe);
    if (d >= 0) {
        sceIoDclose(d);
        cardReady = 1;
    } else if (sceIoMkdir(probe, 0777) >= 0) {
        cardReady = 1;
    }
    port_log("[port] memory card: %s%s\n", cardDir, cardReady ? "" : " (cannot be created: no card)");
    return cardReady;
}

static void pathOf(char* out, const char* name, const char* suffix)
{
    strcpy(out, cardDir);
    strncat(out, name, 31);
    strcat(out, suffix);
}

static int fileSize(const char* name, u32* size)
{
    char path[192];
    pathOf(path, name, "");
    PspIoStat st;
    if (sceIoGetstat(path, &st) < 0) return 0;
    *size = (u32) st.st_size;
    return 1;
}

static s32 done(s32 r) { lastResult = r; return r; }

extern "C" {

void CARDInit(void) { cardInit(); }
s32 CARDGetResultCode(s32 chan) { return lastResult; }

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
    if (chan != 0 || !cardInit()) return CARD_RESULT_NOCARD;
    if (memSize) *memSize = 16;
    if (sectorSize) *sectorSize = SECTOR;
    return CARD_RESULT_READY;
}
s32 CARDMountAsync(s32 chan, void* workArea, CardCallback detachCallback, CardCallback attachCallback)
{
    return done(chan == 0 && cardInit() ? CARD_RESULT_READY : CARD_RESULT_NOCARD);
}
s32 CARDUnmount(s32 chan) { return done(chan == 0 && cardReady ? CARD_RESULT_READY : CARD_RESULT_NOCARD); }
s32 CARDCheckAsync(s32 chan, CardCallback callback) { return done(chan == 0 && cardReady ? CARD_RESULT_READY : CARD_RESULT_NOCARD); }
s32 CARDGetSerialNo(s32 chan, u64* serialNo)
{
    if (chan != 0 || !cardReady) return done(CARD_RESULT_NOCARD);
    *serialNo = 0x5245345053500001ull;
    return done(CARD_RESULT_READY);
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)
{
    if (chan != 0 || !cardReady) return done(CARD_RESULT_NOCARD);
    // the card's 251 blocks minus what the files hold (5 system blocks)
    u32 used = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].used) used += (files[i].length + SECTOR - 1) / SECTOR;
    }
    s32 freeBlocks = (s32) (CARD_BLOCKS - 5) - (s32) used;
    if (freeBlocks < 0) freeBlocks = 0;
    if (byteNotUsed) *byteNotUsed = freeBlocks * SECTOR;
    if (filesNotUsed) *filesNotUsed = 127;
    return done(CARD_RESULT_READY);
}

static int slotOf(const char* name, int create)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].used && strncmp(files[i].name, name, 31) == 0) return i;
    }
    if (!create) return -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!files[i].used) {
            files[i].used = 1;
            strncpy(files[i].name, name, 31);
            files[i].name[31] = 0;
            return i;
        }
    }
    return -1;
}

s32 CARDOpen(s32 chan, const char* fileName, CardFileInfo* fileInfo)
{
    if (chan != 0 || !cardReady) return done(CARD_RESULT_NOCARD);
    u32 size;
    if (!fileSize(fileName, &size)) return done(CARD_RESULT_NOFILE);
    int slot = slotOf(fileName, 1);
    if (slot < 0) return done(CARD_RESULT_FATAL_ERROR);
    files[slot].length = size;
    memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->chan = chan;
    fileInfo->fileNo = slot;
    fileInfo->length = (s32) size;
    return done(CARD_RESULT_READY);
}
s32 CARDClose(CardFileInfo* fileInfo) { return done(CARD_RESULT_READY); }

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CardFileInfo* fileInfo, CardCallback callback)
{
    if (chan != 0 || !cardReady) return done(CARD_RESULT_NOCARD);
    u32 have;
    if (fileSize(fileName, &have)) return done(CARD_RESULT_EXIST);
    char path[192];
    pathOf(path, fileName, "");
    SceUID_ fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd < 0) return done(CARD_RESULT_IOERROR);
    static u8 zero[SECTOR];
    memset(zero, 0, sizeof(zero));
    for (u32 left = size; left > 0;) {
        u32 n = left > SECTOR ? SECTOR : left;
        if (sceIoWrite(fd, zero, n) != (int) n) { sceIoClose(fd); return done(CARD_RESULT_IOERROR); }
        left -= n;
    }
    sceIoClose(fd);
    int slot = slotOf(fileName, 1);
    if (slot < 0) return done(CARD_RESULT_FATAL_ERROR);
    files[slot].length = size;
    CardStat st;
    memset(&st, 0, sizeof(st));
    strncpy(st.fileName, fileName, 31);
    st.length = size;
    memcpy(st.gameName, "G4BE", 4);
    memcpy(st.company, "08", 2);
    st.iconAddr = 0xFFFFFFFF;
    st.commentAddr = 0xFFFFFFFF;
    pathOf(path, fileName, ".stat");
    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, &st, sizeof(st)); sceIoClose(fd); }
    memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->chan = chan;
    fileInfo->fileNo = slot;
    fileInfo->length = (s32) size;
    return done(CARD_RESULT_READY);
}

s32 CARDDeleteAsync(s32 chan, const char* fileName, CardCallback callback)
{
    if (chan != 0 || !cardReady) return done(CARD_RESULT_NOCARD);
    char path[192];
    pathOf(path, fileName, "");
    if (sceIoRemove(path) < 0) return done(CARD_RESULT_NOFILE);
    pathOf(path, fileName, ".stat");
    sceIoRemove(path);
    int slot = slotOf(fileName, 0);
    if (slot >= 0) files[slot].used = 0;
    return done(CARD_RESULT_READY);
}

s32 CARDFormatAsync(s32 chan, CardCallback callback)
{
    if (chan != 0 || !cardReady) return done(CARD_RESULT_NOCARD);
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].used) CARDDeleteAsync(chan, files[i].name, NULL);
    }
    // files not opened in this session: the known save names
    char name[32];
    CARDDeleteAsync(chan, "bh4_system", NULL);
    for (int i = 0; i < 20; i++) {
        sprintf(name, "bh4_data%02d", i);
        CARDDeleteAsync(chan, name, NULL);
    }
    return done(CARD_RESULT_READY);
}

static s32 transfer(CardFileInfo* fileInfo, void* buf, s32 length, s32 offset, int write)
{
    if (!cardReady || fileInfo->fileNo < 0 || fileInfo->fileNo >= MAX_FILES || !files[fileInfo->fileNo].used) return done(CARD_RESULT_NOFILE);
    char path[192];
    pathOf(path, files[fileInfo->fileNo].name, "");
    SceUID_ fd = sceIoOpen(path, write ? PSP_O_RDWR : PSP_O_RDONLY, 0);
    if (fd < 0) return done(CARD_RESULT_NOFILE);
    sceIoLseek(fd, offset, 0);
    int n = write ? sceIoWrite(fd, buf, (unsigned) length) : sceIoRead(fd, buf, (unsigned) length);
    sceIoClose(fd);
    if (n < 0) return done(CARD_RESULT_IOERROR);
    if (!write && n < length) memset((u8*) buf + n, 0, (size_t) (length - n));
    fileInfo->offset = offset + length;
    return done(CARD_RESULT_READY);
}
s32 CARDReadAsync(CardFileInfo* fileInfo, void* buf, s32 length, s32 offset, CardCallback callback) { return transfer(fileInfo, buf, length, offset, 0); }
s32 CARDWriteAsync(CardFileInfo* fileInfo, void* buf, s32 length, s32 offset, CardCallback callback) { return transfer(fileInfo, buf, length, offset, 1); }

s32 CARDGetStatus(s32 chan, s32 fileNo, CardStat* stat)
{
    if (chan != 0 || !cardReady) return done(CARD_RESULT_NOCARD);
    if (fileNo < 0 || fileNo >= MAX_FILES || !files[fileNo].used) return done(CARD_RESULT_NOFILE);
    char path[192];
    pathOf(path, files[fileNo].name, ".stat");
    SceUID_ fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    memset(stat, 0, sizeof(*stat));
    if (fd >= 0) {
        sceIoRead(fd, stat, sizeof(*stat));
        sceIoClose(fd);
    } else {
        strncpy(stat->fileName, files[fileNo].name, 31);
        stat->iconAddr = 0xFFFFFFFF;
        stat->commentAddr = 0xFFFFFFFF;
    }
    stat->length = files[fileNo].length;
    return done(CARD_RESULT_READY);
}
s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CardStat* stat, CardCallback callback)
{
    if (chan != 0 || !cardReady) return done(CARD_RESULT_NOCARD);
    if (fileNo < 0 || fileNo >= MAX_FILES || !files[fileNo].used) return done(CARD_RESULT_NOFILE);
    char path[192];
    pathOf(path, files[fileNo].name, ".stat");
    SceUID_ fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd < 0) return done(CARD_RESULT_IOERROR);
    sceIoWrite(fd, stat, sizeof(*stat));
    sceIoClose(fd);
    return done(CARD_RESULT_READY);
}

}  // extern "C"
