#pragma once

// ============================================================================
//  Shared match cadence so single-player, multiplayer and Virus all run at
//  identical speed/feel. Tron's cells/sec = BASE_STEP_TICKS * MATCH_TICK_MS,
//  so both modes must use the same tick to move at the same rate.
//
//  Every runner reads these through match_shell.h -- change a number here and
//  all three follow. Nothing should hard-code a derived value.
// ============================================================================

#define MATCH_INPUT_MS      40     // input sample interval (~25 Hz)
#define MATCH_TICK_MS       40     // host simulation tick

// ---- countdown ----
#define MATCH_COUNTDOWN_MS     1200   // total 3-2-1 before play
#define MATCH_COUNTDOWN_STEPS  3      // digits counted down; step = MS / STEPS

// ---- result screen ----
#define MATCH_RESULT_MS         3000  // total time on the result screen
#define MATCH_RESULT_HOLD_MS    1200  // hold the final board before standings
#define MATCH_RESULT_LOCKOUT_MS 500   // ignore button presses this long, so the
                                      // keypress that ended the match does not
                                      // immediately skip the result
