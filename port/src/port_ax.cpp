// port/src/port_ax: the AX voice mixer and the MIX volume library (dolphin/ax.h, dolphin/mix.h),
// which the game's sound driver (game/snd_*.cpp) programs, as a software mixer on a PSP audio
// thread. Every 5 ms (an AX frame, 160 samples at 32 kHz) the registered callback runs the driver
// (voice manager, stream player, sequencer) and the running voices are decoded — GameCube
// DSP-ADPCM from the ARAM buffer, or PCM — resampled by their source ratio, scaled by the MIX
// volumes and summed; the 32 kHz frames are resampled to the PSP's 44.1 kHz output.
//
// The driver reads three things straight out of a voice block: pb.state, pb.addr.loopFlag and the
// current / end addresses as one u32 at &pb.addr.currentAddressHi (big-endian order on the
// GameCube), so those are kept in the "u32 view" here. Not reproduced: the aux (reverb) sends, the
// low-pass filter, the volume envelope, the sample-rate converter's 4-tap modes.
#include "types.h"
#include "port.h"
#include "port_psp.h"
#include "port_stub.h"
#define _DOLPHIN_TYPES_H_
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#include <dolphin/ax.h>
#include <dolphin/mix.h>
#include <math.h>
#include <string.h>

extern "C" unsigned char* port_aram_ptr(void);
extern "C" void port_intr_set_bypass(int on);
#ifdef __PSP__
extern "C" void port_log(const char* fmt, ...);
#endif

#define AX_FRAME 160         // samples per 5 ms at 32 kHz
#define AX_RATE 32000
#define OUT_RATE 44100
#define OUT_BLOCK 1024       // frames per sceAudio block (a multiple of 64)
#define RING_FRAMES 8192     // mixed 32 kHz frames waiting for output

struct MixVoice {
    u8 used, running, format, loopFlag, srcType;
    u32 cur, loopAddr, endAddr;     // ADPCM: nibble addresses; PCM16: sample; PCM8: byte
    s16 coef[16];
    u8 predScale;
    s16 yn1, yn2;
    u8 loopPredScale;
    s16 loopYn1, loopYn2;
    u32 ratio;                      // Q16 source rate / 32 kHz
    u32 frac;                       // Q16 position between prev (the current sample) and last (the next)
    s16 prev, last;
    u8 prevEnded, lastEnded;        // the sample is past the end of a one-shot
    // MIX
    int input, auxA, auxB, pan, span, fader;
    f32 gainL, gainR;
    u8 mixUsed;
};

static AXVPB vpb[AX_MAX_VOICES] __attribute__((aligned(32)));
static MixVoice mv[AX_MAX_VOICES];
static AXCallback frameCallback;
static u32 axMode;
static u32 mixMode = MIX_SOUND_MODE_STEREO;
static int axInited;
static SceUID_ audioThread;
static int audioChannel = -1;
static s16 ring[RING_FRAMES * 2];
static u32 ringWrite, ringRead;     // frame indices (mod RING_FRAMES)
static u32 outFrac;                 // Q16 position of the output resampler
static s16 outBlock[2][OUT_BLOCK * 2] __attribute__((aligned(64)));

static inline s16 clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16) v; }

static inline void storeAddr(u16* hiSlot, u32 addr)
{
    memcpy(hiSlot, &addr, 4);  // the driver reads it as one u32 at &...Hi
}

// One source sample of voice `v`; past the end of a one-shot it is 0 with *ended set.
static inline s16 nextSample(MixVoice* v, int* ended)
{
    *ended = 0;
    const u8* aram = port_aram_ptr();
    for (;;) {
        if (v->format == 0 && (v->cur & 15) == 0) {  // DSP-ADPCM: 16-nibble frames, header byte then 14 samples
            v->predScale = aram[(v->cur >> 1) & 0x7FFFFF];
            v->cur += 2;
        }
        if (v->cur < v->endAddr) break;
        if (!v->loopFlag) {
            *ended = 1;
            return 0;
        }
        v->cur = v->loopAddr;
        if (v->format == 0) {
            v->predScale = v->loopPredScale;
            v->yn1 = v->loopYn1;
            v->yn2 = v->loopYn2;
        }
    }
    if (v->format == 0) {
        u8 b = aram[(v->cur >> 1) & 0x7FFFFF];
        int n = (v->cur & 1) ? (b & 0xF) : (b >> 4);
        v->cur++;
        if (n > 7) n -= 16;
        int scale = 1 << (v->predScale & 0xF);
        int ci = (v->predScale >> 4) & 7;
        int s = (n * scale << 11) + 1024 + v->coef[ci * 2] * v->yn1 + v->coef[ci * 2 + 1] * v->yn2;
        s16 out = clamp16(s >> 11);
        v->yn2 = v->yn1;
        v->yn1 = out;
        return out;
    }
    if (v->format == 0x0A) {  // PCM16, big-endian in ARAM
        u32 o = (v->cur * 2) & 0x7FFFFE;
        v->cur++;
        return (s16) ((aram[o] << 8) | aram[o + 1]);
    }
    // PCM8
    s8 s = (s8) aram[v->cur & 0x7FFFFF];
    v->cur++;
    return (s16) (s << 8);
}

static void mixFrame(s16* out)
{
    int accL[AX_FRAME], accR[AX_FRAME];
    memset(accL, 0, sizeof(accL));
    memset(accR, 0, sizeof(accR));
    for (int i = 0; i < AX_MAX_VOICES; i++) {
        MixVoice* v = &mv[i];
        AXVPB* p = &vpb[i];
        if (!v->used || !v->running || p->pb.state != 1) continue;
        int gl = (int) (v->gainL * 4096.0f), gr = (int) (v->gainR * 4096.0f);
        for (int n = 0; n < AX_FRAME; n++) {
            while (v->frac >= 0x10000) {
                int ended;
                v->prev = v->last;
                v->prevEnded = v->lastEnded;
                v->last = nextSample(v, &ended);
                v->lastEnded = (u8) ended;
                v->frac -= 0x10000;
            }
            if (v->prevEnded) {  // the current sample is past the end: the voice stops
                v->running = 0;
                p->pb.state = 0;
                break;
            }
            int s = v->srcType == 0 ? v->prev : v->prev + (((v->last - v->prev) * (int) v->frac) >> 16);
            v->frac += v->ratio;
            accL[n] += (s * gl) >> 12;
            accR[n] += (s * gr) >> 12;
        }
        storeAddr(&p->pb.addr.currentAddressHi, v->cur);
    }
    for (int n = 0; n < AX_FRAME; n++) {
        if (mixMode == MIX_SOUND_MODE_MONO) {
            s16 m = clamp16((accL[n] + accR[n]) >> 1);
            out[n * 2] = m;
            out[n * 2 + 1] = m;
        } else {
            out[n * 2] = clamp16(accL[n]);
            out[n * 2 + 1] = clamp16(accR[n]);
        }
    }
}

static void runFrame(void)
{
    if (frameCallback) {
        port_intr_set_bypass(1);  // the driver's frame runs like the DSP interrupt: whole
        frameCallback();
        port_intr_set_bypass(0);
    }
    s16 frame[AX_FRAME * 2];
    mixFrame(frame);
    for (int n = 0; n < AX_FRAME; n++) {
        u32 w = (ringWrite + n) % RING_FRAMES;
        ring[w * 2] = frame[n * 2];
        ring[w * 2 + 1] = frame[n * 2 + 1];
    }
    ringWrite = (ringWrite + AX_FRAME) % RING_FRAMES;
}

static inline u32 ringAvail(void) { return (ringWrite - ringRead + RING_FRAMES) % RING_FRAMES; }

static int audioThreadMain(unsigned int args, void* argp)
{
    const u32 step = (u32) ((u64) AX_RATE * 65536 / OUT_RATE);
    int which = 0;
    for (;;) {
        s16* out = outBlock[which];
        for (int n = 0; n < OUT_BLOCK; n++) {
            while (ringAvail() < 2) runFrame();
            u32 r0 = ringRead, r1 = (ringRead + 1) % RING_FRAMES;
            int f = (int) (outFrac & 0xFFFF);
            out[n * 2] = (s16) (ring[r0 * 2] + (((ring[r1 * 2] - ring[r0 * 2]) * f) >> 16));
            out[n * 2 + 1] = (s16) (ring[r0 * 2 + 1] + (((ring[r1 * 2 + 1] - ring[r0 * 2 + 1]) * f) >> 16));
            outFrac += step;
            ringRead = (ringRead + (outFrac >> 16)) % RING_FRAMES;
            outFrac &= 0xFFFF;
        }
        sceAudioOutputPannedBlocking(audioChannel, 0x8000, 0x8000, out);
        which ^= 1;
    }
    return 0;
}

extern "C" {

// ---- AX
void AXInitEx(u32 mode)
{
    if (axInited) return;
    axInited = 1;
    memset(vpb, 0, sizeof(vpb));
    memset(mv, 0, sizeof(mv));
    for (int i = 0; i < AX_MAX_VOICES; i++) vpb[i].index = (u32) i;
    audioChannel = sceAudioChReserve(-1, OUT_BLOCK, 0);
    if (audioChannel < 0) {
        port_log("[port] sceAudioChReserve failed (%x): no sound\n", audioChannel);
        return;
    }
    audioThread = sceKernelCreateThread("re4audio", audioThreadMain, 18, 0x8000, PSP_THREAD_ATTR_USER, NULL);
    if (audioThread >= 0) sceKernelStartThread(audioThread, 0, NULL);
}
void AXInit(void) { AXInitEx(0); }
void AXQuit(void) {}
AXCallback AXRegisterCallback(AXCallback callback)
{
    AXCallback old = frameCallback;
    frameCallback = callback;
    return old;
}
void AXSetMode(u32 mode) { axMode = mode; }
u32 AXGetMode(void) { return axMode; }
u32 AXGetMaxDspCycles(void) { return 0; }
u32 AXGetDspCycles(void) { return 0; }
void AXSetMaxDspCycles(u32 cycles) {}
void AXSetCompressor(u32 sw) {}
void AXRegisterAuxACallback(void (*callback)(void*, void*), void* context) {}
void AXRegisterAuxBCallback(void (*callback)(void*, void*), void* context) {}
void AXFXSetHooks(void* (*alloc)(u32), void (*free_)(void*)) {}

AXVPB* AXAcquireVoice(u32 priority, void (*callback)(void*), u32 userContext)
{
    for (int i = 0; i < AX_MAX_VOICES; i++) {
        if (!mv[i].used) {
            MixVoice* v = &mv[i];
            AXVPB* p = &vpb[i];
            memset(v, 0, sizeof(*v));
            memset(&p->pb, 0, sizeof(p->pb));
            v->used = 1;
            v->ratio = 0x10000;
            v->frac = 0x20000;  // fetch the first sample and the next at once
            p->priority = priority;
            p->callback = callback;
            p->userContext = userContext;
            p->index = (u32) i;
            p->pb.state = 0;
            return p;
        }
    }
    return NULL;
}
void AXFreeVoice(AXVPB* p)
{
    if (!p) return;
    mv[p->index].used = 0;
    mv[p->index].running = 0;
    p->pb.state = 0;
}
void AXSetVoicePriority(AXVPB* p, u32 priority) { p->priority = priority; }
void AXSetVoiceState(AXVPB* p, u16 state)
{
    p->pb.state = state;
    mv[p->index].running = state == 1;
}
void AXSetVoiceType(AXVPB* p, u16 type) { p->pb.type = type; }
void AXSetVoiceMix(AXVPB* p, AXPBMIX* mix) { p->pb.mix = *mix; }
void AXSetVoiceSrcType(AXVPB* p, u32 type) { mv[p->index].srcType = (u8) type; p->pb.srcSelect = (u16) type; }
void AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr)
{
    MixVoice* v = &mv[p->index];
    v->loopFlag = (u8) addr->loopFlag;
    v->format = (u8) addr->format;
    v->loopAddr = ((u32) addr->loopAddressHi << 16) | addr->loopAddressLo;
    v->endAddr = ((u32) addr->endAddressHi << 16) | addr->endAddressLo;
    v->cur = ((u32) addr->currentAddressHi << 16) | addr->currentAddressLo;
    v->frac = 0x20000;
    v->prev = v->last = 0;
    v->prevEnded = v->lastEnded = 0;
    p->pb.addr.loopFlag = addr->loopFlag;
    p->pb.addr.format = addr->format;
    storeAddr(&p->pb.addr.loopAddressHi, v->loopAddr);
    storeAddr(&p->pb.addr.endAddressHi, v->endAddr);
    storeAddr(&p->pb.addr.currentAddressHi, v->cur);
}
void AXSetVoiceLoop(AXVPB* p, u16 loop) { mv[p->index].loopFlag = (u8) loop; p->pb.addr.loopFlag = loop; }
void AXSetVoiceLoopAddr(AXVPB* p, u32 addr) { mv[p->index].loopAddr = addr; storeAddr(&p->pb.addr.loopAddressHi, addr); }
void AXSetVoiceEndAddr(AXVPB* p, u32 addr) { mv[p->index].endAddr = addr; storeAddr(&p->pb.addr.endAddressHi, addr); }
void AXSetVoiceCurrentAddr(AXVPB* p, u32 addr) { mv[p->index].cur = addr; storeAddr(&p->pb.addr.currentAddressHi, addr); }
void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* adpcm)
{
    MixVoice* v = &mv[p->index];
    for (int i = 0; i < 16; i++) v->coef[i] = (s16) adpcm->a[i / 2][i % 2];
    v->predScale = (u8) adpcm->pred_scale;
    v->yn1 = (s16) adpcm->yn1;
    v->yn2 = (s16) adpcm->yn2;
    p->pb.adpcm = *adpcm;
}
void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* loop)
{
    MixVoice* v = &mv[p->index];
    v->loopPredScale = (u8) loop->loop_pred_scale;
    v->loopYn1 = (s16) loop->loop_yn1;
    v->loopYn2 = (s16) loop->loop_yn2;
    p->pb.adpcmLoop = *loop;
}
void AXSetVoiceSrc(AXVPB* p, AXPBSRC* src)
{
    MixVoice* v = &mv[p->index];
    v->ratio = ((u32) src->ratioHi << 16) | src->ratioLo;
    if (v->ratio == 0) v->ratio = 0x10000;
    p->pb.src = *src;
}
void AXSetVoiceSrcRatio(AXVPB* p, f32 ratio)
{
    u32 r = (u32) (ratio * 65536.0f);
    mv[p->index].ratio = r ? r : 0x10000;
}
void AXSetVoiceLpf(AXVPB* p, AXPBLPF* lpf) { p->pb.lpf = *lpf; }
void AXSetVoiceLpfCoefs(AXVPB* p, u16 a0, u16 b0) { p->pb.lpf.a0 = a0; p->pb.lpf.b0 = b0; }
void AXGetLpfCoefs(u16 freq, u16* a0, u16* b0) { *a0 = 0; *b0 = 0; }

// ---- MIX: volumes in 0.1 dB (0 = unity, -904 = silence), pan 0 (left) .. 127 (right)
static f32 dbGain(int db)
{
    if (db <= -904) return 0.0f;
    if (db >= 0) return 1.0f;
    return powf(10.0f, (f32) db / 200.0f);
}
static void mixUpdate(MixVoice* v)
{
    f32 g = dbGain(v->input) * dbGain(v->fader);
    f32 t = (f32) (v->pan < 0 ? 0 : v->pan > 127 ? 127 : v->pan) / 127.0f * 1.5707963f;
    v->gainL = g * cosf(t);
    v->gainR = g * sinf(t);
}
void MIXInit(void) {}
void MIXQuit(void) {}
void MIXSetSoundMode(u32 mode) { mixMode = mode; }
u32 MIXGetSoundMode(void) { return mixMode; }
void MIXInitChannel(AXVPB* p, u32 mode, int input, int auxA, int auxB, int pan, int span, int fader)
{
    MixVoice* v = &mv[p->index];
    v->mixUsed = 1;
    v->input = input; v->auxA = auxA; v->auxB = auxB; v->pan = pan; v->span = span; v->fader = fader;
    mixUpdate(v);
}
void MIXReleaseChannel(AXVPB* p) { if (p) mv[p->index].mixUsed = 0; }
void MIXResetControls(AXVPB* p) { MixVoice* v = &mv[p->index]; v->input = 0; v->auxA = -904; v->auxB = -904; v->pan = 64; v->span = 127; v->fader = 0; mixUpdate(v); }
void MIXSetInput(AXVPB* p, int dB) { MixVoice* v = &mv[p->index]; v->input = dB; mixUpdate(v); }
void MIXAdjustInput(AXVPB* p, int dB) { MixVoice* v = &mv[p->index]; v->input += dB; mixUpdate(v); }
int MIXGetInput(AXVPB* p) { return mv[p->index].input; }
void MIXSetAuxA(AXVPB* p, int dB) { mv[p->index].auxA = dB; }
void MIXAdjustAuxA(AXVPB* p, int dB) { mv[p->index].auxA += dB; }
int MIXGetAuxA(AXVPB* p) { return mv[p->index].auxA; }
void MIXSetAuxB(AXVPB* p, int dB) { mv[p->index].auxB = dB; }
void MIXAdjustAuxB(AXVPB* p, int dB) { mv[p->index].auxB += dB; }
int MIXGetAuxB(AXVPB* p) { return mv[p->index].auxB; }
void MIXSetPan(AXVPB* p, int pan) { MixVoice* v = &mv[p->index]; v->pan = pan; mixUpdate(v); }
void MIXAdjustPan(AXVPB* p, int pan) { MixVoice* v = &mv[p->index]; v->pan += pan; mixUpdate(v); }
int MIXGetPan(AXVPB* p) { return mv[p->index].pan; }
void MIXSetSPan(AXVPB* p, int span) { mv[p->index].span = span; }
void MIXAdjustSPan(AXVPB* p, int span) { mv[p->index].span += span; }
int MIXGetSPan(AXVPB* p) { return mv[p->index].span; }
void MIXSetFader(AXVPB* p, int dB) { MixVoice* v = &mv[p->index]; v->fader = dB; mixUpdate(v); }
void MIXAdjustFader(AXVPB* p, int dB) { MixVoice* v = &mv[p->index]; v->fader += dB; mixUpdate(v); }
int MIXGetFader(AXVPB* p) { return mv[p->index].fader; }
void MIXAuxAPostFader(AXVPB* p) {}
void MIXAuxAPreFader(AXVPB* p) {}
int MIXAuxAIsPostFader(AXVPB* p) { return 0; }
void MIXAuxBPostFader(AXVPB* p) {}
void MIXAuxBPreFader(AXVPB* p) {}
int MIXAuxBIsPostFader(AXVPB* p) { return 0; }
void MIXUpdateSettings(void) {}

// Host test hook: runs one AX frame into `out` (160 stereo frames) without the audio thread.
void port_ax_test_frame(s16* out) { mixFrame(out); }

}  // extern "C"
