#ifndef PORT_BE_H
#define PORT_BE_H

// Big-endian fields of the game's file-resident structures. The disc's data is big-endian and the
// engine reads it in place through its struct layouts; on the little-endian PSP the port declares
// those fields with these types, which store the bytes as the file has them and swap on every
// access, so the engine's own loaders keep working on unconverted files. The matching build sees
// plain typedefs. Pointer-typed fields that hold a file offset until the loader relocates them are
// read through FILE_U32 at the relocation, and the "already relocated" tests through IS_RELOCATED.
#if !defined(RE4_PORT) || !defined(__cplusplus)  // the matching build, and the port's C units (no file structs)
typedef u16 be_u16;
typedef s16 be_s16;
typedef u32 be_u32;
typedef s32 be_s32;
typedef f32 be_f32;
#define FILE_U32(x) (u32) x
#define FILE_U16(x) (u16) x
#define FILE_S16(x) (s16) x
#ifndef RE4_PORT
#define IS_RELOCATED(p) (s32) p < 0     // pointers into MEM1 are negative, file offsets are not
#define NOT_RELOCATED(p) (s32) p >= 0
#else
#define IS_RELOCATED(p) GC_PTR_OK(p)
#define NOT_RELOCATED(p) !GC_PTR_OK(p)
#endif
#else
#define FILE_U32(x) __builtin_bswap32((u32) (x))
#define FILE_U16(x) ((u16) __builtin_bswap16((u16) (x)))
#define FILE_S16(x) ((s16) __builtin_bswap16((u16) (x)))
#define IS_RELOCATED(p) GC_PTR_OK(p)
#define NOT_RELOCATED(p) !GC_PTR_OK(p)

class be_u32 {
    u32 raw;
public:
    operator u32() const { return __builtin_bswap32(raw); }
    be_u32& operator=(u32 v) { raw = __builtin_bswap32(v); return *this; }
    be_u32& operator+=(u32 v) { return *this = (u32) *this + v; }
    be_u32& operator-=(u32 v) { return *this = (u32) *this - v; }
    be_u32& operator|=(u32 v) { return *this = (u32) *this | v; }
    be_u32& operator&=(u32 v) { return *this = (u32) *this & v; }
    be_u32& operator++() { return *this += 1; }
    be_u32& operator--() { return *this -= 1; }
    u32 operator++(int) { u32 o = *this; *this += 1; return o; }
    u32 operator--(int) { u32 o = *this; *this -= 1; return o; }
};
class be_s32 {
    u32 raw;
public:
    operator s32() const { return (s32) __builtin_bswap32(raw); }
    be_s32& operator=(s32 v) { raw = __builtin_bswap32((u32) v); return *this; }
    be_s32& operator+=(s32 v) { return *this = (s32) *this + v; }
    be_s32& operator-=(s32 v) { return *this = (s32) *this - v; }
    be_s32& operator|=(s32 v) { return *this = (s32) *this | v; }
    be_s32& operator&=(s32 v) { return *this = (s32) *this & v; }
    be_s32& operator++() { return *this += 1; }
    be_s32& operator--() { return *this -= 1; }
    s32 operator++(int) { s32 o = *this; *this += 1; return o; }
    s32 operator--(int) { s32 o = *this; *this -= 1; return o; }
};
class be_u16 {
    u16 raw;
public:
    operator u16() const { return __builtin_bswap16(raw); }
    be_u16& operator=(u16 v) { raw = __builtin_bswap16(v); return *this; }
    be_u16& operator+=(u16 v) { return *this = (u16) ((u16) *this + v); }
    be_u16& operator-=(u16 v) { return *this = (u16) ((u16) *this - v); }
    be_u16& operator|=(u16 v) { return *this = (u16) ((u16) *this | v); }
    be_u16& operator&=(u16 v) { return *this = (u16) ((u16) *this & v); }
    be_u16& operator++() { return *this += 1; }
    be_u16& operator--() { return *this -= 1; }
    u16 operator++(int) { u16 o = *this; *this += 1; return o; }
    u16 operator--(int) { u16 o = *this; *this -= 1; return o; }
};
class be_s16 {
    u16 raw;
public:
    operator s16() const { return (s16) __builtin_bswap16(raw); }
    be_s16& operator=(s16 v) { raw = __builtin_bswap16((u16) v); return *this; }
    be_s16& operator+=(s16 v) { return *this = (s16) ((s16) *this + v); }
    be_s16& operator-=(s16 v) { return *this = (s16) ((s16) *this - v); }
    be_s16& operator++() { return *this += 1; }
    be_s16& operator--() { return *this -= 1; }
};
class be_f32 {
    u32 raw;
public:
    operator f32() const { u32 v = __builtin_bswap32(raw); f32 f; __builtin_memcpy(&f, &v, 4); return f; }
    be_f32& operator=(f32 f) { u32 v; __builtin_memcpy(&v, &f, 4); raw = __builtin_bswap32(v); return *this; }
    be_f32& operator+=(f32 v) { return *this = (f32) *this + v; }
    be_f32& operator-=(f32 v) { return *this = (f32) *this - v; }
    be_f32& operator*=(f32 v) { return *this = (f32) *this * v; }
};
#endif

#endif
