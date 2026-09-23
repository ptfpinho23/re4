// port/src/port_yz2: Capcom's yz2 decoder (game/yz2asm.cpp, PowerPC assembly in the original) in
// C. yz2code.cpp builds the models and the dictionary and calls yz2Decode_Decode; the stream is an
// adaptive range coder: a 0x500-symbol main model (0x000..0x1FF short references with an
// explicit length, 0x200..0x3FF long references that reuse the stored run length, 0x400..0x4FF
// literal bytes) and a 0x100-symbol model for the length fields. References index a 512-entry ring
// of (start, length) runs kept per context byte (the byte before the run). The ring stores run
// starts as offsets from the output start (the original kept pointers; offsets keep the unit
// host-testable).
//
// A stream whose header says packed size 0 is raw (tools/port/gamedata.py writes archives that way
// once it has converted their contents): the payload is copied as is.
//
// tools/port/yz2.py is the same reading of the assembly in Python, with an encoder; its self-test
// compiles this unit on the host and checks the two decoders against each other.
#include "types.h"
#include "port.h"
#include <string.h>

struct Yz2InEv {
    u8* src;
    u8* heap;
    u8* free;
    u32 size0;
    u32 size1;
};
struct Yz2Freq {
    u16* freq;
    u32 max;
    int bits;
    u32 range;
    u16* cum;   // pairs: [2i] frequency, [2i + 1] cumulative
    int n;
    u32 acc;
};
struct Yz2Dec;
struct Yz2Model {
    Yz2Dec* dec;
    Yz2Freq fd;
    u8* table;  // u32[0x8000]: cumulative position -> symbol
};
struct Yz2Dec {
    u32 mask;   // the range
    u32 byte;   // the code window
    Yz2Model m1;
    Yz2Model m2;
};
struct Yz2DicEnt {
    u32 cnt;
    u32 start[0x200];  // offsets from the output start
    u32 length[0x200];
};
struct Yz2Ctx {
    Yz2DicEnt* dic;
    Yz2Dec d;
    u32 pad_54;
    int n;
};

struct Coder {
    Yz2Dec* dec;
    u8** src;
};

// One symbol of `m` (FrequencyDecode_Decode / _Decode768).
static u32 decodeSymbol(Yz2Model* m, Yz2Dec* dec, u8** srcp, int* tableValid)
{
    u32 R = dec->mask;
    u32 C = dec->byte;
    u8* src = *srcp;
    if (R <= 0x800000) {
        if (R > 0x8000) {
            C = (C << 8) | src[0];
            src += 1;
            R <<= 8;
        } else if (R > 0x80) {
            C = (C << 16) | ((u32) src[0] << 8) | src[1];
            src += 2;
            R <<= 16;
        } else {
            C = (C << 24) | ((u32) src[0] << 16) | ((u32) src[1] << 8) | src[2];
            src += 3;
            R <<= 24;
        }
    }
    *srcp = src;
    R >>= 14;
    dec->byte = C;
    dec->mask = R;
    u32 target = C / R;
    Yz2Freq* f = &m->fd;
    u32* table = (u32*) m->table;
    if (!*tableValid) {
        for (int i = 0; i < f->n; i++) {
            u32 fr = f->cum[i * 2];
            u32 lo = f->cum[i * 2 + 1];
            for (u32 j = lo; j < lo + fr; j++) {
                table[j] = (u32) i;
            }
        }
        *tableValid = 1;
    }
    u32 sym = table[target];
    u32 fr = f->cum[sym * 2];
    u32 lo = f->cum[sym * 2 + 1];
    dec->byte = C - R * lo;
    dec->mask = (R * fr) >> 1;
    f->freq[sym] += 1;
    f->max += 1;
    if (f->bits <= 14) {
        if (f->max == f->range) {
            u32 mult = 1u << (15 - f->bits);
            u32 acc = 0;
            *tableValid = 0;
            for (int i = 0; i < f->n; i++) {
                u32 v = f->freq[i] * mult;
                f->cum[i * 2 + 1] = (u16) acc;
                acc += v;
                f->cum[i * 2] = (u16) v;
            }
            f->bits += 1;
            f->range = 1u << f->bits;
        }
    } else if (f->max > 0x7FFF) {
        u32 acc = 0;
        *tableValid = 0;
        f->max = 0;
        for (int i = 0; i < f->n; i++) {
            u32 v = f->freq[i];
            f->cum[i * 2] = (u16) v;
            f->cum[i * 2 + 1] = (u16) acc;
            acc += v;
            if (v > 1) {
                v >>= 1;
                f->freq[i] = (u16) v;
            }
            f->max += v;
        }
    }
    return sym;
}

extern "C" void yz2Decode_Decode(void* ctxp, void* dstp, u32 size, void* evp)
{
    Yz2Ctx* ctx = (Yz2Ctx*) ctxp;
    Yz2InEv* ev = (Yz2InEv*) evp;
    u8* out = (u8*) dstp;
    u8* end = out + size;
    u8* src = ev->src;
    if (ev->size0 == 0) {  // raw archive from the pipeline (Yz2DecodeExec already took the first byte)
        memcpy(out, src - 1, size);
        ev->src = src - 1 + size;
        return;
    }
    Yz2DicEnt* dic = ctx->dic;
    Yz2Dec* dec = &ctx->d;
    Yz2Model* m1 = &dec->m1;
    Yz2Model* m2 = &dec->m2;
    int valid1 = 0, valid2 = 0;
    u8* scan = out;  // the position whose following run is not in the dictionary yet
    while (out < end) {
        u32 sym = decodeSymbol(m1, dec, &src, &valid1);
        u32 len;
        if (sym > 0x3FF) {
            *out++ = (u8) sym;
            len = 1;
        } else {
            Yz2DicEnt* ent = &dic[*scan];
            u32 idx = (sym + (ent->cnt & 0xFFFF)) & 0x1FF;
            const u8* from;
            if (sym > 0x1FF) {
                len = ent->length[idx];
                from = (const u8*) dstp + ent->start[idx];
            } else {
                u32 t = decodeSymbol(m2, dec, &src, &valid2);
                if (t > 2) {
                    len = t;
                } else if (t == 2) {
                    u32 a = decodeSymbol(m2, dec, &src, &valid2) << 8;
                    len = a | decodeSymbol(m2, dec, &src, &valid2);
                } else if (t == 1) {
                    u32 a = decodeSymbol(m2, dec, &src, &valid2) << 16;
                    a |= decodeSymbol(m2, dec, &src, &valid2) << 8;
                    len = a | decodeSymbol(m2, dec, &src, &valid2);
                } else {
                    u32 a = decodeSymbol(m2, dec, &src, &valid2) << 24;
                    a |= decodeSymbol(m2, dec, &src, &valid2) << 16;
                    a |= decodeSymbol(m2, dec, &src, &valid2) << 8;
                    len = a | decodeSymbol(m2, dec, &src, &valid2);
                }
                from = (const u8*) dstp + ent->start[idx];
                len -= 1;
            }
            for (u32 i = 0; i < len; i++) {  // forward copy: overlapping runs repeat
                out[i] = from[i];
            }
            out += len;
        }
        // enter the run just written under its context byte
        if (scan < out - 1) {
            u8 b = *scan;
            u8* start = scan + 1;
            scan = out - 1;
            Yz2DicEnt* ent = &dic[b];
            u32 c = ent->cnt;
            ent->start[c] = (u32) (start - (u8*) dstp);
            ent->length[c] = len;
            ent->cnt = (c + 1) & 0x1FF;
        }
    }
    ev->src = src;
}
