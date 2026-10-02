// wep00 module: the hand "weapon" (no weapon equipped). Creates the cObjHand object, gives the
// player the hand model and fills the player's motion table with the hand motions of the weapon
// archive.

#include "wep_mod.h"

cObjWep* equipWeapon(cPlayer* pl);

// The motion table stores are plain `pl->m_MotTbl[i] = ...` (global.h WEP_MOT); the pG reload after each
// one is the compiler's own (mem-flags patch).

// WeaponInitFunc of the module (cPlayer::weaponInit -> pl_wep.cpp calls it with the player):
// creates the hand object, stores it as the player's weapon (Wep->m_pWep) and installs the hand
// motions. (The error strings still say Wep13: copied from the launcher module.)
static void Wep00_init(cModel* m)
{
    cPlayer* pl = (cPlayer*) m;
    cObjWep* obj = equipWeapon(pl);

    if (obj == 0) {
        pLog->err(0, 0, "Wep13_init() failed.");
    } else {
        pl->Wep->m_pWep = obj;
        obj->setMotion(pl);
    }
}

// WeaponMoveFunc of the module: the empty hand has no weapon routine (pl_R1_Weapon never runs
// anything for it).
void Wep00_move(cPlayer* pl)
{
}

// Creates the cObjHand (ObjMgr id 0x3D), inits it on the player and gives the player the bare
// hand model from the player archive (0x12); the hands are only re-set when Status_flg[1] bit21
// (an event holds the hand models) is clear. Returns the object, NULL when the work is full.
cObjWep* equipWeapon(cPlayer* pl)
{
    cObjWep* obj = (cObjWep*) ObjMgr.createBack(cObjMgr::ID_WEP_HAND);

    if (obj == 0) {
        pLog->err(0, 0, "Wep13_init() cObjWep CREATE FAILED");
        return 0;
    }
    obj->init(pl);
    pl->Body->initWepHand((u32) PL_ARC_PTR(pG->pPlayer, 0x12));
    if (!StaFlagChk(pG, STA_PL_BOAT)) {
        pl->setRightHand(1);
        pl->setLeftHand(0);
    }
    return obj;
}

// ObjInitFunc[0x3D]: placement-constructs the hand object in the work cObjMgr::construct hands over.
void ObjHand_init(cObj* obj)
{
    new (obj) cObjHand;
}

// Fills the player's motion table (m_MotTbl) with the unarmed footwork set of the weapon archive
// (idle, walk, run, turns, back, the 0x39..0x42 damage set, 0x57/0x5B knife transitions; 0x3D
// stays the player archive's). Called from Wep00_init and PlReloadBullet.
void cObjHand::setMotion(cPlayer* pl)
{
    WEP_MOT(pl, 0x00, 0x04);
    WEP_MOT(pl, 0x02, 0x05);
    WEP_MOT(pl, 0x03, 0x0B);
    WEP_MOT(pl, 0x04, 0x05);
    WEP_MOT(pl, 0x05, 0x05);
    WEP_MOT(pl, 0x06, 0x07);
    WEP_MOT(pl, 0x07, 0x0D);
    WEP_MOT(pl, 0x08, 0x06);
    WEP_MOT(pl, 0x09, 0x0C);
    WEP_MOT(pl, 0x0A, 0x06);
    WEP_MOT(pl, 0x0B, 0x08);
    WEP_MOT(pl, 0x0C, 0x0E);
    WEP_MOT(pl, 0x0D, 0x09);
    WEP_MOT(pl, 0x0E, 0x0F);
    WEP_MOT(pl, 0x0F, 0x0A);
    WEP_MOT(pl, 0x10, 0x10);
    WEP_MOT(pl, 0x12, 0x07);
    WEP_MOT(pl, 0x13, 0x07);
    WEP_MOT(pl, 0x39, 0x13);
    WEP_MOT(pl, 0x3A, 0x14);
    PLA_MOT(pl, 0x3D, 0x5D);
    WEP_MOT(pl, 0x41, 0x15);
    WEP_MOT(pl, 0x42, 0x16);
    WEP_MOT(pl, 0x3F, 0x11);
    WEP_MOT(pl, 0x40, 0x12);
    WEP_MOT(pl, 0x5B, 0x17);
    WEP_MOT(pl, 0x57, 0x18);
}

// REL entry: registers the module's weapon init / move routines and the object constructor slot.
extern "C" void _prolog()
{
    WeaponInitFunc = Wep00_init;
    WeaponMoveFunc = Wep00_move;
    ObjInitFunc[0x3D] = ObjHand_init;
    OSReport("Wep00 HAND prolog Ok\n");
}

// REL exit: frees the object constructor slot (the weapon function pointers are replaced by the next module).
extern "C" void _epilog()
{
    ObjInitFunc[0x3D] = 0;
}

// Target of every unresolved cross-module branch (snmakerel patches them to `bl _unresolved`).
extern "C" void _unresolved()
{
}
