// Host test build: drives the port's AX mixer (port/src/port_ax.cpp) with a command script on
// stdin and prints the mixed frames; tools/port/axtest.py writes the scripts and checks them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#define _DOLPHIN_TYPES_H_
#define TRUE 1
#define FALSE 0
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#include <dolphin/ax.h>
#include <dolphin/mix.h>

static unsigned char aram[0x100000];
extern "C" unsigned char* port_aram_ptr(void) { return aram; }
extern "C" void port_intr_set_bypass(int on) {}
extern "C" void port_ax_test_frame(s16* out);

int main()
{
    static AXVPB* voices[64];
    static char line[1 << 16];
    while (fgets(line, sizeof(line), stdin)) {
        char cmd[32];
        if (sscanf(line, "%31s", cmd) != 1) continue;
        char* args = line + strlen(cmd);
        while (*args == ' ') args++;
        if (!strcmp(cmd, "aram")) {
            unsigned ofs; char* hex = (char*) malloc(strlen(args) + 1);
            sscanf(args, "%x %s", &ofs, hex);
            for (int i = 0; hex[2 * i] && hex[2 * i + 1]; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); aram[ofs + i] = (u8) v; }
            free(hex);
        } else if (!strcmp(cmd, "voice")) {
            int prio; sscanf(args, "%d", &prio);
            AXVPB* v = AXAcquireVoice((u32) prio, NULL, 0);
            if (!v) { printf("VOICE none\n"); continue; }
            voices[v->index] = v;
            printf("VOICE %u\n", (unsigned) v->index);
        } else if (!strcmp(cmd, "free")) {
            int i; sscanf(args, "%d", &i); AXFreeVoice(voices[i]); voices[i] = NULL;
        } else if (!strcmp(cmd, "addr")) {
            int i; unsigned lf, fmt, lp, en, cur; sscanf(args, "%d %u %u %u %u %u", &i, &lf, &fmt, &lp, &en, &cur);
            AXPBADDR a; a.loopFlag = (u16) lf; a.format = (u16) fmt;
            a.loopAddressHi = (u16) (lp >> 16); a.loopAddressLo = (u16) lp; a.endAddressHi = (u16) (en >> 16); a.endAddressLo = (u16) en;
            a.currentAddressHi = (u16) (cur >> 16); a.currentAddressLo = (u16) cur;
            AXSetVoiceAddr(voices[i], &a);
        } else if (!strcmp(cmd, "adpcm")) {
            int i, c[16], ps, y1, y2;
            sscanf(args, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &i, &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7], &c[8], &c[9], &c[10], &c[11], &c[12], &c[13], &c[14], &c[15], &ps, &y1, &y2);
            AXPBADPCM a; for (int k = 0; k < 16; k++) a.a[k / 2][k % 2] = (u16) c[k];
            a.gain = 0; a.pred_scale = (u16) ps; a.yn1 = (u16) y1; a.yn2 = (u16) y2;
            AXSetVoiceAdpcm(voices[i], &a);
        } else if (!strcmp(cmd, "adpcmloop")) {
            int i, ps, y1, y2; sscanf(args, "%d %d %d %d", &i, &ps, &y1, &y2);
            AXPBADPCMLOOP l; l.loop_pred_scale = (u16) ps; l.loop_yn1 = (u16) y1; l.loop_yn2 = (u16) y2;
            AXSetVoiceAdpcmLoop(voices[i], &l);
        } else if (!strcmp(cmd, "ratio")) {
            int i; unsigned r; sscanf(args, "%d %u", &i, &r);
            AXPBSRC s; memset(&s, 0, sizeof(s)); s.ratioHi = (u16) (r >> 16); s.ratioLo = (u16) r; AXSetVoiceSrc(voices[i], &s);
        } else if (!strcmp(cmd, "srctype")) {
            int i, t; sscanf(args, "%d %d", &i, &t); AXSetVoiceSrcType(voices[i], (u32) t);
        } else if (!strcmp(cmd, "mix")) {
            int i, in, aa, ab, pan, span, fader; sscanf(args, "%d %d %d %d %d %d %d", &i, &in, &aa, &ab, &pan, &span, &fader);
            MIXInitChannel(voices[i], 0, in, aa, ab, pan, span, fader);
        } else if (!strcmp(cmd, "state")) {
            int i, st; sscanf(args, "%d %d", &i, &st); AXSetVoiceState(voices[i], (u16) st);
        } else if (!strcmp(cmd, "frame")) {
            s16 out[160 * 2];
            port_ax_test_frame(out);
            printf("F");
            for (int k = 0; k < 160 * 2; k++) printf(" %d", out[k]);
            printf("\n");
        } else if (!strcmp(cmd, "status")) {
            int i; sscanf(args, "%d", &i);
            u32 cur; memcpy(&cur, &voices[i]->pb.addr.currentAddressHi, 4);
            printf("STATUS %u %u\n", (unsigned) voices[i]->pb.state, (unsigned) cur);
        } else { fprintf(stderr, "unknown command %s\n", cmd); return 1; }
    }
    return 0;
}
