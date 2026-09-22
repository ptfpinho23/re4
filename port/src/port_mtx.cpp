// port/src/port_mtx: the paired-single matrix / vector / quaternion entry points bound to the
// SDK's C implementations (build/port/sdk_mtx.c, extracted by tools/port/extract_sdk_c.py), plus
// the one function that only ever existed as paired-single code.
#include "types.h"
#include "port.h"
#include <dolphin/mtx.h>

extern "C" {

void PSMTXIdentity(Mtx m) { C_MTXIdentity(m); }
void PSMTXCopy(const Mtx src, Mtx dst) { C_MTXCopy(src, dst); }
void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) { C_MTXConcat(a, b, ab); }
void PSMTXTranspose(const Mtx src, Mtx xPose) { C_MTXTranspose(src, xPose); }
u32 PSMTXInverse(const Mtx src, Mtx inv) { return C_MTXInverse(src, inv); }
void PSMTXRotRad(Mtx m, char axis, f32 rad) { C_MTXRotRad(m, axis, rad); }
void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) { C_MTXRotAxisRad(m, axis, rad); }
void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT) { C_MTXTrans(m, xT, yT, zT); }
void PSMTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT) { C_MTXTransApply(src, dst, xT, yT, zT); }
void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS) { C_MTXScale(m, xS, yS, zS); }
void PSMTXQuat(Mtx m, const Quaternion* q) { C_MTXQuat(m, q); }
void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst) { C_MTXMultVec(m, src, dst); }
void PSMTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) { C_MTXMultVecArray(m, srcBase, dstBase, count); }
void PSMTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) { C_MTXMultVecSR(m, src, dst); }
void PSMTX44MultVec(const Mtx44 m, const Vec* src, Vec* dst) { C_MTX44MultVec(m, src, dst); }

// Reordered (column-major 3x4) matrix for the skinning kernels: dest[j][i] = src[i][j].
void PSMTXReorder(const Mtx src, ROMtx dest)
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            dest[j][i] = src[i][j];
        }
    }
}

void PSVECAdd(const Vec* a, const Vec* b, Vec* ab) { C_VECAdd(a, b, ab); }
void PSVECSubtract(const Vec* a, const Vec* b, Vec* a_b) { C_VECSubtract(a, b, a_b); }
void PSVECScale(const Vec* src, Vec* dst, f32 scale) { C_VECScale(src, dst, scale); }
void PSVECNormalize(const Vec* src, Vec* dst) { C_VECNormalize(src, dst); }
f32 PSVECSquareMag(const Vec* v) { return C_VECSquareMag(v); }
f32 PSVECMag(const Vec* v) { return C_VECMag(v); }
f32 PSVECDotProduct(const Vec* a, const Vec* b) { return C_VECDotProduct(a, b); }
void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* axb) { C_VECCrossProduct(a, b, axb); }
f32 PSVECSquareDistance(const Vec* a, const Vec* b) { return C_VECSquareDistance(a, b); }
f32 PSVECDistance(const Vec* a, const Vec* b) { return C_VECDistance(a, b); }

}  // extern "C"
