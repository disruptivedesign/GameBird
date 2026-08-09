#pragma once
#include <FastLED.h>
#include "core/display.h"

// ============================================================================
//  Shared screen chrome. Nothing here calls show() -- the caller does, so a
//  screen can compose several of these before pushing one frame.
// ============================================================================

// Evenly spaced selector dots along one row: `n` positions with `sel`
// highlighted. Every list on the device draws these, and they all used to do it
// with their own copy of the same spacing arithmetic against a hard-coded 8.
void drawSelectorDots(Display& d, int row, int n, int sel,
                      const CRGB& on  = CRGB(255, 255, 255),
                      const CRGB& off = CRGB(0, 40, 40));
