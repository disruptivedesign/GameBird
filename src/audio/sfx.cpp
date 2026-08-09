#include "sfx.h"
#include "notes.h"

// ============================================================================
//  The catalog. Every array is file-static and const: it lives in flash and
//  outlives any Sfx copied from it, which is the lifetime Audio::play needs.
//
//  Keep edits inside the two rules in sfx.h -- C5..C7, and <= 150 ms for
//  anything that fires during play.
// ============================================================================

// ---- shell / navigation ----------------------------------------------------
// Navigation blips are deliberately tiny. Holding the stick auto-repeats at
// JOY_REPEAT_MS, so these fire around nine times a second while scrubbing a
// list; anything with a tail would smear into a drone.
static const Step S_MOVE[]   = { {NOTE_E6, 18} };
static const Step S_SELECT[] = { {NOTE_C6, 28}, {NOTE_G6, 44} };   // rising = in
static const Step S_BACK[]   = { {NOTE_G6, 28}, {NOTE_C6, 44} };   // falling = out

const Sfx SFX_MOVE   = SFX_OF(S_MOVE,   PRIO_UI);
const Sfx SFX_SELECT = SFX_OF(S_SELECT, PRIO_UI);
const Sfx SFX_BACK   = SFX_OF(S_BACK,   PRIO_UI);

// ---- match moments ---------------------------------------------------------
static const Step S_BOOT[] = {
  {NOTE_C6, 70}, {NOTE_E6, 70}, {NOTE_G6, 70}, {NOTE_C7, 150},
};
// The countdown digit and GO are the same interval an octave apart, so "go"
// reads as the resolution of the three ticks rather than a separate noise.
static const Step S_COUNTDOWN[] = { {NOTE_E6,  70} };
static const Step S_GO[]        = { {NOTE_E7, 160} };

static const Step S_WIN[] = {
  {NOTE_C6, 80}, {NOTE_E6, 80}, {NOTE_G6, 80}, {NOTE_C7, 120}, {REST, 40}, {NOTE_C7, 200},
};
static const Step S_LOSE[] = {
  {NOTE_G5, 110}, {NOTE_E5, 110}, {NOTE_C5, 260},
};

const Sfx SFX_BOOT      = SFX_OF(S_BOOT,      PRIO_MATCH);
const Sfx SFX_COUNTDOWN = SFX_OF(S_COUNTDOWN, PRIO_MATCH);
const Sfx SFX_GO        = SFX_OF(S_GO,        PRIO_MATCH);
const Sfx SFX_WIN       = SFX_OF(S_WIN,       PRIO_MATCH);
const Sfx SFX_LOSE      = SFX_OF(S_LOSE,      PRIO_MATCH);

// ---- gameplay defaults -----------------------------------------------------
// These are the ones that can fire every tick, so they are the shortest things
// in the file. SFX_HIT in particular may be the most-played sound on the device.
static const Step S_HIT[]   = { {NOTE_G6, 16} };
static const Step S_GROW[]  = { {NOTE_C7, 14} };
static const Step S_SPORE[] = { {NOTE_E6, 25}, {NOTE_B6, 35} };
static const Step S_DEATH[] = { {NOTE_A6, 40}, {NOTE_D6, 70} };

const Sfx SFX_HIT   = SFX_OF(S_HIT,   PRIO_MINOR);
const Sfx SFX_GROW  = SFX_OF(S_GROW,  PRIO_MINOR);
const Sfx SFX_SPORE = SFX_OF(S_SPORE, PRIO_MAJOR);
const Sfx SFX_DEATH = SFX_OF(S_DEATH, PRIO_MAJOR);
