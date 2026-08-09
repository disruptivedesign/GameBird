#pragma once
#include <stdint.h>

// ============================================================================
//  The sound data model -- the vocabulary everything else speaks.
//
//  DELIBERATELY FREE OF <Arduino.h>. virus_rules.cpp is compiled by the native
//  test env (see platformio.ini) against the shims in test/shims, and it is
//  where virus authors declare their voices. If this header ever pulls in the
//  Arduino core, `pio test -e native` stops building. Keep it stdint-only.
// ============================================================================

// One step of a sound: hold `freq` Hz for `ms` milliseconds. freq == 0 is a
// REST -- silence for that long without ending the sound, which is how a
// stinger gets a gap between notes.
struct Step {
  uint16_t freq;
  uint16_t ms;
};

// Who wins the buzzer. There is one piezo, so there is one voice, and the only
// interesting question in the whole audio system is what happens when two
// things want it at once (see Audio::play).
enum SfxPriority : uint8_t {
  PRIO_UI    = 0,   // menu move, select, back
  PRIO_MINOR = 1,   // a cell grew, a hit landed
  PRIO_MAJOR = 2,   // you lost a cell, you spored
  PRIO_MATCH = 3,   // countdown, go, win, lose -- nothing outranks the match
};

// A playable sound: a run of steps plus how hard it will fight for the buzzer.
//
// `steps` must have static storage duration. Audio copies the Sfx by value but
// NOT the array it points at, so a `const Step[]` at file scope is right and a
// local temporary is a dangling pointer. Every catalog entry follows that shape.
struct Sfx {
  const Step* steps;
  uint8_t     count;
  uint8_t     priority;
};

// Build an Sfx from a Step[] literal without hand-counting it:
//   static const Step MY_BLIP[] = { {NOTE_E6, 30} };
//   const Sfx SFX_MY_BLIP = SFX_OF(MY_BLIP, PRIO_UI);
#define SFX_OF(arr, prio) \
  Sfx{ (arr), (uint8_t)(sizeof(arr) / sizeof((arr)[0])), (uint8_t)(prio) }
