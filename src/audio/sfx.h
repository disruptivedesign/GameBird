#pragma once
#include "sound.h"

// ============================================================================
//  The device's shared voice: the sounds the shell uses and games inherit, so
//  GAMEBIRD sounds like one product rather than five. Games are free to define
//  their own Step arrays (Virus authors do -- see virus_rules.cpp), but taking
//  these first keeps navigation and match moments consistent everywhere.
//
//  Like sound.h, this stays free of <Arduino.h>: sfx.cpp is compiled by the
//  native test env so virus_rules.cpp can reference the catalog off-hardware.
//
//  Two rules the catalog follows, both from src/audio/notes.h and audio.cpp:
//    - Everything sits in C5..C7, where the piezo is actually loud.
//    - In-game stingers stay <= 150 ms, comfortably inside one Virus tick
//      (180 ms), so a sound never outlives the moment it describes.
// ============================================================================

// ---- shell / navigation (PRIO_UI) ----
extern const Sfx SFX_MOVE;      // menu selection moved
extern const Sfx SFX_SELECT;    // entered / confirmed
extern const Sfx SFX_BACK;      // backed out

// ---- match moments (PRIO_MATCH -- nothing outranks these) ----
extern const Sfx SFX_BOOT;      // splash
extern const Sfx SFX_COUNTDOWN; // one countdown digit
extern const Sfx SFX_GO;        // countdown over, play begins
extern const Sfx SFX_WIN;
extern const Sfx SFX_LOSE;

// ---- gameplay defaults ----
extern const Sfx SFX_HIT;       // PRIO_MINOR -- you landed damage
extern const Sfx SFX_GROW;      // PRIO_MINOR -- you claimed ground
extern const Sfx SFX_SPORE;     // PRIO_MAJOR -- you leapt a blockade
extern const Sfx SFX_DEATH;     // PRIO_MAJOR -- you lost a cell
