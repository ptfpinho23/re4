// game/cons.cpp: per-room constants ("cons" table): the work pool sizes the room start uses
// (game.cpp: 0 enemies, 1 objects, 2 esp, 3 espgen, 4 ctrl, 5 lights, 6 / 7 model and parts
// pools, 8 primitives, 10 / 11 collision pieces). A room may override entries; the rest come
// from ConsRoomDefault.

#include "types.h"
#include "db_log.h"

// Per-room constants: a count, a validity bitmap and the values.
// ConsInitCore is unused in this build (only its message survives in .rodata).
struct ConsRoom {     // file-resident (the room's CONS sub-file): big-endian fields
    be_u32 num;
    be_u32 bits[1];   // (num >> 5) + 1 words, followed by u32 values[num]
};

static u32 ConsRoomDefault[12] = {
    60, 200, 1024, 256, 10, 100, 1300, 300, 0xA0000, 10, 30, 30,
};

ConsRoom* pConsRoom;

// Pointer check of the original table loader (unused here, see above).
static inline int ConsInitCore(void* p)
{
    if (p == 0) {
        pLog->err(0, 0, "ConsInitCore() INVALID PTR %08x", p);
        return 0;
    }
    return 1;
}

// Installs the room's constants table (NULL = defaults only).
int ConsInitRoom(ConsRoom* p)
{
    pConsRoom = p;
    return 1;
}

// Constant `no`: the room's value when its table has the entry (validity bit set), else the
// default.
u32 ConsGetRoomValue(u32 id)
{
    ConsRoom* r = pConsRoom;
    be_u32* bits;
    be_u32* values;

    if (r == 0) {
        return ConsRoomDefault[id];
    }
    bits = r->bits;
    values = &r->bits[(r->num >> 5) + 1];
    if (id >= r->num || !(bits[id >> 5] & (1 << (id & 31)))) {
        return ConsRoomDefault[id];
    }
    return values[id];
}
