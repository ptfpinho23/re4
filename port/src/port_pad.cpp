// port/src/port_pad: the GameCube controller on the PSP's buttons and stick.
//
// Mapping: cross A, circle B, triangle X, square Y, start START, select Z, L / R the triggers,
// the D-pad itself, the stick to the main stick. The C-stick and analog triggers have no PSP
// equivalent yet (the camera and aiming layer will decide what to do with them).
#include "types.h"
#include "port.h"
#include "port_psp.h"
#include <dolphin/pad.h>
#include <string.h>

extern "C" {

BOOL PADInit(void)
{
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(1);  // PSP_CTRL_MODE_ANALOG
    return TRUE;
}

u32 PADRead(PADStatus* status)
{
    PspCtrlData d;
    memset(status, 0, sizeof(PADStatus) * 4);
    memset(&d, 0, sizeof(d));
    sceCtrlPeekBufferPositive(&d, 1);
    u16 b = 0;
    if (d.buttons & PSP_CTRL_CROSS) b |= PAD_BUTTON_A;
    if (d.buttons & PSP_CTRL_CIRCLE) b |= PAD_BUTTON_B;
    if (d.buttons & PSP_CTRL_TRIANGLE) b |= PAD_BUTTON_X;
    if (d.buttons & PSP_CTRL_SQUARE) b |= PAD_BUTTON_Y;
    if (d.buttons & PSP_CTRL_START) b |= PAD_BUTTON_START;
    if (d.buttons & PSP_CTRL_SELECT) b |= PAD_TRIGGER_Z;
    if (d.buttons & PSP_CTRL_LTRIGGER) b |= PAD_TRIGGER_L;
    if (d.buttons & PSP_CTRL_RTRIGGER) b |= PAD_TRIGGER_R;
    if (d.buttons & PSP_CTRL_UP) b |= PAD_BUTTON_UP;
    if (d.buttons & PSP_CTRL_DOWN) b |= PAD_BUTTON_DOWN;
    if (d.buttons & PSP_CTRL_LEFT) b |= PAD_BUTTON_LEFT;
    if (d.buttons & PSP_CTRL_RIGHT) b |= PAD_BUTTON_RIGHT;
    status[0].button = b;
    status[0].stickX = (s8) ((int) d.lx - 128);
    status[0].stickY = (s8) (127 - (int) d.ly);
    status[0].triggerLeft = (d.buttons & PSP_CTRL_LTRIGGER) ? 255 : 0;
    status[0].triggerRight = (d.buttons & PSP_CTRL_RTRIGGER) ? 255 : 0;
    status[0].err = PAD_ERR_NONE;
    status[1].err = status[2].err = status[3].err = PAD_ERR_NO_CONTROLLER;
    return 1;
}

void PADClamp(PADStatus* status) {}
void PADControlMotor(s32 chan, u32 command) {}
BOOL PADReset(u32 mask) { return TRUE; }
BOOL PADRecalibrate(u32 mask) { return TRUE; }
void PADSetAnalogMode(u32 mode) {}

}  // extern "C"
