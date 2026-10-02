// wep35 module: a hand "weapon" (cObjHand, no weapon model). Creates the object, gives the
// player the hand model and fills the player's motion table from the weapon archive.

#include "wep_mod.h"

cObjWep* equipWeapon(cPlayer* pl);

// The motion table stores are plain `pl->m_MotTbl[i] = ...` (global.h WEP_MOT); the pG reload after each
// one is the compiler's own (mem-flags patch).
// One of the four per-character empty-hand modules (wep34..wep37, the same code with the
// character's hand model / motion table); like wep00 there is no weapon routine.

// WeaponInitFunc of the module (cPlayer::weaponInit with the player): creates the hand object,
// stores it as Wep->m_pWep and installs the hand motions. (The error string still says Wep13.)
static void Wep35_init(cModel* m)
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

// WeaponMoveFunc: the empty hand has no weapon routine.
void Wep35_move(cPlayer* pl)
{
}

// Creates the cObjHand (ObjMgr id 0x3D), inits it on the player and gives the player the bare
// hand model from the player archive (0x12; right hand 1, left hand 0). NULL when the work is full.
cObjWep* equipWeapon(cPlayer* pl)
{
    cObjWep* obj = (cObjWep*) ObjMgr.createBack(cObjMgr::ID_WEP_HAND);

    if (obj == 0) {
        pLog->err(0, 0, "Wep35_init() cObjWep CREATE FAILED");
        return 0;
    }
    obj->init(pl);
    pl->Body->initWepHand((u32) PL_ARC_PTR(pG->pPlayer, 0x12));
    pl->setRightHand(1);
    pl->setLeftHand(0);
    return obj;
}

// ObjInitFunc[0x3D]: placement-constructs the hand object in the work cObjMgr::construct hands over.
void ObjHand_init(cObj* obj)
{
    new (obj) cObjHand;
}

// Fills the player's motion table (m_MotTbl) with the unarmed footwork set of the weapon archive
// (idle, walk, run, turns, back, 0x12/0x13; no damage set; 0x3D stays the player archive's).
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
    PLA_MOT(pl, 0x3D, 0x5D);
}

// REL entry: registers the weapon init / move routines and the object constructor slot.
extern "C" void _prolog()
{
    WeaponInitFunc = Wep35_init;
    WeaponMoveFunc = Wep35_move;
    ObjInitFunc[0x3d] = ObjHand_init;
    OSReport("Wep35 HAND prolog Ok\n");
}

// REL exit: frees the object constructor slot.
extern "C" void _epilog()
{
    ObjInitFunc[0x3d] = 0;
}

// Target of every unresolved cross-module branch (snmakerel patches them to `bl _unresolved`).
extern "C" void _unresolved()
{
}
