// port/src/port_dvd: the disc, as a directory of files on the memory stick.
//
// The game's files live under <root>/data/ with the disc's own tree (bgm/bio4str.hed,
// etc/core.das, st1/r100.das, ...), the two discs merged into one tree. The root is the EBOOT's
// own directory: ms0:/PSP/GAME/RE4/ on a PSP and in PPSSPP's memory stick, umd0:/ or host0:/
// when PPSSPP boots the EBOOT from elsewhere. Paths are looked up on demand (sceIoGetstat) and
// remembered in a table whose index is the entry number the game keeps. Reads are synchronous;
// the callback runs before DVDReadAsyncPrio returns, which the game's read queue (dvd.cpp) copes
// with since it only polls flags the callback sets.
#include "types.h"
#include "port.h"
#include "port_psp.h"
#include <dolphin/os.h>
#include <dolphin/dvd.h>
#include <string.h>
#include <stdlib.h>

#define ENTRY_MAX 4096
#define PATH_MAX_ 128

struct Entry {
    char path[PATH_MAX_];  // disc path as the game spelled it
    u32 length;
};

static char dataRoot[64];     // e.g. "ms0:/PSP/GAME/RE4/data/"
static Entry* entries;        // allocated on first use
static u32 entryCount;
static int openFd = -1;       // the file the last read came from
static s32 openEntry = -1;
static int currentDisc;       // 0 = disc 1
static DVDDiskID currentId;
static int dvdTrace = 1;      // log every read
// The game's threads and the sound driver's stream player (on the audio thread) read at the same
// time; the disc driver queued them, this serialises them.
static SceUID_ dvdSema = -1;
static void dvdLock(void)
{
    if (dvdSema < 0) dvdSema = sceKernelCreateSema("re4dvd", 0, 1, 1, NULL);
    sceKernelWaitSema(dvdSema, 1, NULL);
}
static void dvdUnlock(void) { sceKernelSignalSema(dvdSema, 1); }

extern "C" void port_demo_run(void);  // port_demo.cpp

static const char* const roots[] = {"ms0:/PSP/GAME/RE4/", "umd0:/", "host0:/", "disc0:/", "ms0:/RE4/", NULL};

// A dummy for DVDGetFSTLocation (dvd.cpp only prints it).
static u32 fakeFst[4];

static int findRoot(void)
{
    if (dataRoot[0]) {
        return 1;
    }
    for (int i = 0; roots[i]; i++) {
        char path[96];
        strcpy(path, roots[i]);
        strcat(path, "data");
        int d = sceIoDopen(path);
        if (d >= 0) {
            sceIoDclose(d);
            strcpy(dataRoot, path);
            strcat(dataRoot, "/");
            port_log("[port] data directory: %s\n", dataRoot);
            // the core archive is the first thing the game needs; without it the tree is not the game
            strcpy(path, dataRoot);
            strcat(path, "etc/core.das");
            PspIoStat st;
            if (sceIoGetstat(path, &st) < 0) {
                port_log("[port] *** GAME FILES MISSING: %setc/core.das not found ***\n", dataRoot);
                port_log("[port] extract both discs with tools/port/gamedata.py extract <disc.iso> data\n");
                port_log("[port] and copy the data/ tree next to this EBOOT; the game cannot start without it\n");
                port_demo_run();  // shows the renderer instead; never returns
            }
            memset(&currentId, 0, sizeof(currentId));
            memcpy(currentId.gameName, "G4BE", 4);
            memcpy(currentId.company, "08", 2);
            return 1;
        }
    }
    port_log("[port] no data directory: put the game files under ms0:/PSP/GAME/RE4/data/ (umd0:/data and host0:/data are tried too)\n");
    return 0;
}

// The memory card layer keeps its files next to the data tree.
extern "C" const char* port_data_root(void)
{
    findRoot();
    return dataRoot;
}

static void fullPath(char* out, const char* discPath)
{
    while (*discPath == '/') discPath++;
    strcpy(out, dataRoot);
    strncat(out, discPath, PATH_MAX_ - 1);
}

static s32 findEntry(const char* discPath)
{
    while (*discPath == '/') discPath++;
    for (u32 i = 0; i < entryCount; i++) {
        const char* a = entries[i].path;
        const char* b = discPath;
        while (*a && *b) {
            char x = *a, y = *b;
            if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
            if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
            if (x != y) break;
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return (s32) i;
        }
    }
    return -1;
}

extern "C" {

static s32 DVDConvertPathToEntrynum_locked(const char* path);

s32 DVDConvertPathToEntrynum(const char* path)
{
    dvdLock();
    s32 r = DVDConvertPathToEntrynum_locked(path);
    dvdUnlock();
    return r;
}

static s32 DVDConvertPathToEntrynum_locked(const char* path)
{
    if (!findRoot()) {
        return -1;
    }
    s32 e = findEntry(path);
    if (e >= 0) {
        return e;
    }
    char full[192];
    fullPath(full, path);
    PspIoStat st;
    if (sceIoGetstat(full, &st) < 0 || (st.st_mode & 0x1000)) {  // FIO_S_IFDIR
        return -1;
    }
    if (entries == NULL) {
        entries = (Entry*) malloc(sizeof(Entry) * ENTRY_MAX);
        memset(entries, 0, sizeof(Entry) * ENTRY_MAX);
    }
    if (entryCount >= ENTRY_MAX) {
        port_log("[port] DVD entry table full (%d)\n", ENTRY_MAX);
        return -1;
    }
    while (*path == '/') path++;
    strncpy(entries[entryCount].path, path, PATH_MAX_ - 1);
    entries[entryCount].length = (u32) st.st_size;
    return (s32) entryCount++;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo)
{
    if (entrynum < 0 || (u32) entrynum >= entryCount) {
        return FALSE;
    }
    memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->startAddr = (u32) entrynum;  // the entry number stands in for the disc offset
    fileInfo->length = entries[entrynum].length;
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDOpen(const char* fileName, DVDFileInfo* fileInfo)
{
    s32 e = DVDConvertPathToEntrynum(fileName);
    return e >= 0 ? DVDFastOpen(e, fileInfo) : FALSE;
}

BOOL DVDClose(DVDFileInfo* fileInfo) { return TRUE; }

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio)
{
    s32 entry = (s32) fileInfo->startAddr;
    s32 n = -1;
    // No lock around the read itself: the game's scheduler may kill the reading task's thread
    // mid-read (cDvd::ReadNblk2Blk turns a background read into a blocking one), which would
    // leave the lock taken. Each read opens its own descriptor; the table is fixed after findRoot.
    int known = entry >= 0 && (u32) entry < entryCount;
    if (known) {
        char full[192];
        fullPath(full, entries[entry].path);
        int fd = sceIoOpen(full, PSP_O_RDONLY, 0);
        if (fd >= 0) {
            sceIoLseek(fd, offset, PSP_SEEK_SET);
            n = sceIoRead(fd, addr, (u32) length);
            sceIoClose(fd);
            // The game reads 32-byte multiples past the end of a file; the disc had padding there.
            if (n >= 0 && n < length) {
                memset((u8*) addr + n, 0, length - n);
                n = length;
            }
        }
    }
    if (dvdTrace) {
        port_trace("[dvd] read %s ofs %x len %x -> %p: %d\n", known ? entries[entry].path : "?", (unsigned) offset,
                 (unsigned) length, addr, (int) n);
    }
    fileInfo->cb.state = DVD_STATE_END;
    fileInfo->cb.currTransferSize = (u32) length;
    fileInfo->cb.transferredSize = n < 0 ? 0 : (u32) n;
    fileInfo->callback = callback;
    if (callback) {
        callback(n < 0 ? DVD_RESULT_FATAL_ERROR : n, fileInfo);
    }
    return TRUE;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) { return DVD_STATE_END; }
s32 DVDGetTransferredSize(DVDFileInfo* fileinfo) { return (s32) fileinfo->cb.transferredSize; }
s32 DVDGetDriveStatus(void) { return DVD_STATE_END; }

BOOL DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback callback)
{
    if (callback) {
        callback(DVD_RESULT_GOOD, block);
    }
    return TRUE;
}

s32 DVDCancelAll(void) { return 0; }

void* DVDGetFSTLocation(void)
{
    findRoot();
    return fakeFst;
}

DVDDiskID* DVDGetCurrentDiskID(void)
{
    findRoot();
    currentId.diskNumber = (u8) currentDisc;
    return &currentId;
}

DVDDiskID* DVDGenerateDiskID(DVDDiskID* diskID, const char* gameName, const char* company, u8 diskNumber, u8 gameVersion)
{
    memset(diskID, 0, sizeof(*diskID));
    memcpy(diskID->gameName, gameName, 4);
    memcpy(diskID->company, company, 2);
    diskID->diskNumber = diskNumber;
    diskID->gameVersion = gameVersion;
    return diskID;
}

BOOL DVDCompareDiskID(const DVDDiskID* id1, const DVDDiskID* id2)
{
    return memcmp(id1->gameName, id2->gameName, 4) == 0 && memcmp(id1->company, id2->company, 2) == 0 &&
           id1->diskNumber == id2->diskNumber;
}

// Both discs are one tree: a disc change just records the number the game asked for.
BOOL DVDChangeDiskAsync(DVDCommandBlock* block, DVDDiskID* id, DVDCBCallback callback)
{
    currentDisc = id->diskNumber;
    port_log("[port] disc change to %d (no-op: one merged data tree)\n", currentDisc + 1);
    block->state = DVD_STATE_END;
    if (callback) {
        callback(DVD_RESULT_GOOD, block);
    }
    return TRUE;
}

}  // extern "C"
