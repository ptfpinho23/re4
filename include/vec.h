#ifndef VEC_H
#define VEC_H

// Dolphin SDK vector/matrix types and the PS* routines used by game code. The SDK headers in
// include/dolphin/ pull in the CodeWarrior libc and cannot be compiled by ProDG/GCC, so game
// units include this instead. Shares the dolphin/mtx.h guard so both can coexist.

#include "types.h"

#ifndef _DOLPHIN_MTX_H_
#define _DOLPHIN_MTX_H_

typedef struct {
    f32 x, y, z;
} Vec;
// A Vec in file data: big-endian components (port_be.h); the matching build sees a Vec.
#ifdef RE4_PORT
struct BeVec {
    be_f32 x, y, z;
    operator Vec() const { Vec v; v.x = x; v.y = y; v.z = z; return v; }
    BeVec& operator=(const Vec& v) { x = v.x; y = v.y; z = v.z; return *this; }
};
// The address of a file vector where a Vec* is wanted (a read-only argument): the port hands over
// a converted copy (a small ring of them, so several can sit in one call).
static inline Vec* port_bevec_ptr(const BeVec& v)
{
    static Vec ring[8];
    static int next;
    Vec* r = &ring[next++ & 7];
    *r = v;
    return r;
}
#define BEVEC_PTR(v) port_bevec_ptr(v)
#else
typedef Vec BeVec;
#define BEVEC_PTR(v) &v
#endif

typedef f32 Mtx[3][4];
typedef f32 (*MtxPtr)[4];
typedef f32 Mtx44[4][4];

#ifdef __cplusplus
extern "C" {
#endif

void PSMTXIdentity(Mtx m);
void PSMTXCopy(const Mtx src, Mtx dst);
void PSMTXConcat(const Mtx lhs, const Mtx rhs, Mtx ab);
void PSMTXTranspose(const Mtx src, Mtx xPose);
u32 PSMTXInverse(const Mtx src, Mtx inv);
void PSMTXReorder(Mtx src, f32 dst[4][3]);
void PSMTXRotRad(Mtx m, char axis, f32 rad);
void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA);
void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 rad);
void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT);
void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS);
void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst);
void PSMTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count);
void PSMTXMultVecSR(const Mtx m, const Vec* src, Vec* dst);
void PSMTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT);

void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f);
void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f);
void C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f);
void C_MTXLookAt(Mtx m, const Vec* camPos, const Vec* camUp, const Vec* target);
void PSMTX44MultVec(const Mtx44 m, const Vec* src, Vec* dst);
void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT);

void PSVECAdd(const Vec* a, const Vec* b, Vec* ab);
void PSVECSubtract(const Vec* a, const Vec* b, Vec* a_b);
void PSVECScale(const Vec* src, Vec* dst, f32 scale);
void PSVECNormalize(const Vec* src, Vec* unit);
f32 PSVECSquareMag(const Vec* v);
f32 PSVECMag(const Vec* v);
f32 PSVECDotProduct(const Vec* a, const Vec* b);
void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* axb);
f32 PSVECSquareDistance(const Vec* a, const Vec* b);
f32 PSVECDistance(const Vec* a, const Vec* b);
void C_VECReflect(const Vec* src, const Vec* normal, Vec* dst);

typedef struct {
    f32 x, y, z, w;
} Quaternion;

void C_QUATMtx(Quaternion* r, const Mtx m);
void C_QUATSlerp(const Quaternion* p, const Quaternion* q, Quaternion* r, f32 t);
void PSMTXQuat(Mtx m, const Quaternion* q);

#ifdef __cplusplus
}
#endif

#endif

// Mtx copy as the original's inlined word loop (three rows of four f32 through row pointers); a plain
// block. The loop shape is a register-allocation lever: units whose bytes need another one keep a
// local variant (motion.cpp MTX_COPY_DOWN, at_mod.cpp / ss_pzzl.cpp MTX_COPY_DO, sce_at.cpp
// MTX_COPY_LATE_DST), see docs/matching.md "Mtx copy loops".
#define MTX_COPY(src, dst)               \
    {                                    \
        MtxPtr d_ = (dst);               \
        MtxPtr s_ = (src);               \
        int i_ = 3;                      \
        int j_;                          \
        f32* sp_;                        \
        f32* dp_;                        \
        while (i_--) {                   \
            dp_ = *d_;                   \
            sp_ = *s_;                   \
            for (j_ = 0; j_ < 4; j_++) { \
                *dp_++ = *sp_++;         \
            }                            \
            d_++;                        \
            s_++;                        \
        }                                \
    }

#endif
