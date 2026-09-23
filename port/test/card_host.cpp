// Host test of the memory card layer (port/src/port_card.cpp) over a directory: the PSP file
// calls are mapped to POSIX here. Run from tools/port/cardtest.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "types.h"
#include "port_psp.h"
#include "card.h"

static char root[256];
extern "C" const char* port_data_root(void) { return root; }
extern "C" int sceIoOpen(const char* f, int flags, int mode)
{
    int o = (flags & 3) == 1 ? O_RDONLY : (flags & 3) == 2 ? O_WRONLY : O_RDWR;
    if (flags & 0x200) o |= O_CREAT;
    if (flags & 0x400) o |= O_TRUNC;
    return open(f, o, 0666);
}
extern "C" int sceIoClose(int fd) { return close(fd); }
extern "C" int sceIoRead(int fd, void* d, unsigned n) { return (int) read(fd, d, n); }
extern "C" int sceIoWrite(int fd, const void* d, unsigned n) { return (int) write(fd, d, n); }
extern "C" long long sceIoLseek(int fd, long long o, int w) { return lseek(fd, o, w); }
extern "C" int sceIoRemove(const char* f) { return unlink(f); }
extern "C" int sceIoMkdir(const char* d, int m) { return mkdir(d, 0777); }
extern "C" int sceIoDopen(const char* d) { struct stat st; return stat(d, &st) == 0 && S_ISDIR(st.st_mode) ? 1 : -1; }
extern "C" int sceIoDclose(int fd) { return 0; }
extern "C" int sceIoGetstat(const char* f, PspIoStat* s) { struct stat st; if (stat(f, &st)) return -1; s->st_size = st.st_size; return 0; }

int main(int argc, char** argv)
{
    snprintf(root, sizeof(root), "%s/data/", argv[1]);
    s32 mem, sec;
    printf("probe %d", CARDProbeEx(0, &mem, &sec)); printf(" %d %d\n", mem, sec);
    printf("mount %d\n", CARDMountAsync(0, NULL, NULL, NULL));
    s32 bytes, files;
    printf("free %d", CARDFreeBlocks(0, &bytes, &files)); printf(" %d %d\n", bytes, files);
    CardFileInfo fi;
    printf("open-missing %d\n", CARDOpen(0, "bh4_data00", &fi));
    printf("create %d", CARDCreateAsync(0, "bh4_data00", 3 << 13, &fi, NULL)); printf(" no %d len %d\n", fi.fileNo, fi.length);
    u8 buf[0x2000];
    for (int i = 0; i < 0x2000; i++) buf[i] = (u8) (i * 7);
    printf("write %d\n", CARDWriteAsync(&fi, buf, 0x2000, 0x2000, NULL));
    u8 back[0x2000];
    printf("read %d", CARDReadAsync(&fi, back, 0x2000, 0x2000, NULL)); printf(" same %d\n", memcmp(buf, back, 0x2000) == 0);
    printf("read0 %d", CARDReadAsync(&fi, back, 0x2000, 0, NULL)); int z = 1; for (int i = 0; i < 0x2000; i++) if (back[i]) z = 0; printf(" zero %d\n", z);
    CardStat st;
    printf("stat %d", CARDGetStatus(0, fi.fileNo, &st)); printf(" len %u comment %x name %s\n", st.length, st.commentAddr, st.fileName);
    st.commentAddr = 0x40; st.iconAddr = 0x80; st.time = 123456;
    printf("setstat %d\n", CARDSetStatusAsync(0, fi.fileNo, &st, NULL));
    memset(&st, 0, sizeof(st));
    printf("stat2 %d", CARDGetStatus(0, fi.fileNo, &st)); printf(" comment %x icon %x time %u\n", st.commentAddr, st.iconAddr, st.time);
    printf("close %d\n", CARDClose(&fi));
    CardFileInfo fi2;
    printf("reopen %d", CARDOpen(0, "bh4_data00", &fi2)); printf(" len %d\n", fi2.length);
    printf("create-exists %d\n", CARDCreateAsync(0, "bh4_data00", 3 << 13, &fi, NULL));
    printf("delete %d\n", CARDDeleteAsync(0, "bh4_data00", NULL));
    printf("open-deleted %d\n", CARDOpen(0, "bh4_data00", &fi));
    return 0;
}
