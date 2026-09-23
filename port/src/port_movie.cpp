// port/src/port_movie: the CRI Sofdec movie player (include/mwply.h, game/sofdec.cpp) as a player
// that ends every movie on its first frame. The title screen's attract loop and the openings go
// through cSofdec, which calls the handle's interface table, so a NULL handle (the generated
// stub) would crash there; this handle reports MWE_PLY_STAT_PLAYEND at once and hands out no
// frames, and cSofdec releases everything and lets the game go on. Decoding the .sfd streams
// (MPEG-1 video, ADX audio) on the PSP is later work.
#include "types.h"
#include "mwply.h"

extern "C" {

static void plyNop(MWPLY hn) {}
static void plyStart(MWPLY hn, const char* fname) {}
static int plyGetStat(MWPLY hn) { return MWE_PLY_STAT_PLAYEND; }
static void plyGetTime(MWPLY hn, int* ncount, int* tscale)
{
    if (ncount) *ncount = 0;
    if (tscale) *tscale = 1;
}
static void plyPause(MWPLY hn, int sw) {}
static void* plyQuery(MWPLY hn, void* iid) { return NULL; }
static u32 plyRef(MWPLY hn) { return 1; }

static MWPLY_IF plyIf = {plyQuery, plyRef, plyRef, plyNop, plyNop, plyNop, plyStart, plyNop, plyGetStat, plyGetTime, plyPause};
static MWPLY_OBJ plyObj = {&plyIf};

void mwPlyInitSfdFx(MWS_PLY_INIT_SFD* prm) {}
int mwPlyCalcWorkCprmSfd(MWS_PLY_CPRM_SFD* prm) { return 0x1000; }
MWPLY mwPlyCreateSofdec(MWS_PLY_CPRM_SFD* prm) { return &plyObj; }

void mwPlyGetHdrInf(void* buf, int size, MWS_SFD_HDRINF* info)
{
    // the game only wants the frame size, for its texture; the buffer is the file's first 0x5000 bytes
    for (int i = 0; i < (int) sizeof(*info); i++) ((u8*) info)[i] = 0;
    info->width = 512;
    info->height = 448;
}

void mwPlyGetCurFrm(MWPLY hn, MWS_FRM* frm)
{
    for (int i = 0; i < (int) sizeof(*frm); i++) ((u8*) frm)[i] = 0;
    frm->bufadr = NULL;  // no decoded frame
}
void mwPlyRelCurFrm(MWPLY hn) {}
int mwPlyGetNumSkipDec(MWPLY hn) { return 0; }
int mwPlyGetNumSkipDisp(MWPLY hn) { return 0; }
void mwPlyFxSetOutBufSize(MWPLY hn, int width, int height) {}
void mwPlyFxSetOutBufPitchHeight(MWPLY hn, int pitch, int height) {}
void mwPlyFxCnvFrmY84C44(MWPLY hn, MWS_FRM* frm, void* ybuf, void* uvbuf) {}
void mwPlyFxCnvFrmARGB8888(MWPLY hn, MWS_FRM* frm, void* buf) {}

// The CRI ADX manager tick and error hook the player shares.
void ADXM_ExecMain(void) {}
void ADXM_SetCbErr(void (*func)(void* obj, const char* msg), void* obj) {}

}  // extern "C"
