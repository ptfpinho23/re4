#ifndef TPL_H
#define TPL_H

#include "types.h"

// TPL texture palette layout (charPipeline/texPalette.h without the SDK includes). File offsets
// are relocated to pointers by cTexSys::CalcTplAddr.
struct CLUTHeader {
    be_u16 numEntries;  // 0x00
    u8 unpacked;     // 0x02
    u8 pad8;         // 0x03
    be_u32 format;      // 0x04
    void* data;      // 0x08
};

struct TEXHeader {
    be_u16 height;         // 0x00
    be_u16 width;          // 0x02
    be_u32 format;         // 0x04
    void* data;         // 0x08
    be_u32 wrapS;          // 0x0C
    be_u32 wrapT;          // 0x10
    be_u32 minFilter;      // 0x14
    be_u32 magFilter;      // 0x18
    be_f32 LODBias;        // 0x1C
    u8 edgeLODEnable;   // 0x20
    u8 minLOD;          // 0x21
    u8 maxLOD;          // 0x22
    u8 unpacked;        // 0x23
};

struct TEXDescriptor {
    TEXHeader* textureHeader;  // 0x00
    CLUTHeader* CLUTHeader;    // 0x04
};

struct TEXPalette {
    be_u32 version;               // 0x00
    be_u32 numDescriptors;              // 0x04
    TEXDescriptor* descriptorArray;  // 0x08
};

extern "C" TEXDescriptor* TEXGet(TEXPalette* pal, u32 id);

#endif
