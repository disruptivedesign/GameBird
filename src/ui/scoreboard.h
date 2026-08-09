#pragma once
#include <FastLED.h>
#include "core/display.h"

// ============================================================================
//  Shared score presentation. Nothing here calls show() -- the caller does, so
//  a screen can compose several of these before pushing one frame.
// ============================================================================

// Win bars, used by both multiplayer and single-player: one vertical bar per
// player (height = wins, capped to the panel), colored by colors[i]. The
// winner's bar top blinks white. winner == 0xFF for none.
void drawWinBars(Display& d, const uint8_t* scores, const CRGB* colors,
                 int n, uint8_t winner, uint32_t now);

// ---- scrolling a score ------------------------------------------------------
// A panel this small cannot show a score, so every game marquees it instead.
// Both of these draw the number vertically centered and scrolling right-to-left
// at one shared rate, which is the point: the digits crawl past at the same
// speed whichever game you are in.

// One pass, driven by time since the scroll began. Returns true once the number
// has fully left the panel -- the caller's cue to move on.
bool drawScoreScroll(Display& d, uint32_t value, uint32_t elapsedMs, const CRGB& c);

// The same scroll, looping forever, for an idle high-score screen. Takes a
// running clock (millis()) rather than an elapsed time.
void drawScoreScrollLoop(Display& d, uint32_t value, uint32_t nowMs, const CRGB& c);
