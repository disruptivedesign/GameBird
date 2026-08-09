#pragma once
#include "core/display.h"
#include "audio/audio.h"
#include "match_config.h"

// ============================================================================
//  The bits of match presentation every runner shares.
//
//  Multiplayer, SinglePlayer and VirusApp are three different match runners --
//  different lobbies, different scoreboards, different things to do when a
//  match ends -- but they open and close a match identically: a 3-2-1
//  countdown, then a result screen that holds the final board before showing
//  standings. Those two pieces live here so the timing is defined once, in
//  match_config.h, instead of being re-derived in each runner.
// ============================================================================

// Draw the countdown digit for `elapsed` ms into the countdown, clearing and
// showing the panel, and sound one tick each time the digit changes. The caller
// decides when the countdown is over (elapsed >= MATCH_COUNTDOWN_MS).
//
// The beep lives here rather than in the runners for the same reason the digit
// does: three runners open a match, and the countdown should look and sound
// identical in all three. Callers still paint every frame -- the edge detection
// is internal (see match_shell.cpp).
void drawMatchCountdown(Display& disp, Audio& audio, uint32_t elapsed);

// Sound the start of play. Call at the countdown -> playing transition, which
// is the one moment the runners own rather than share.
void playMatchGo(Audio& audio);

// Is the result screen finished? True once it times out, or as soon as the
// player presses something after the lockout. `pressed` is whatever counts as
// a press for that runner -- A only, or A or B.
bool matchResultReleased(uint32_t elapsed, bool pressed);

// True while the result screen should still show the final board rather than
// the standings.
inline bool matchResultShowsBoard(uint32_t elapsed) {
  return elapsed < MATCH_RESULT_HOLD_MS;
}
