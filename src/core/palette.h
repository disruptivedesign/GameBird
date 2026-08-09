#pragma once
#include <FastLED.h>

// ============================================================================
//  Shared user-selectable colors. One home for the Settings color picker, the
//  lobby swatches, and the games' default player palette. A device stores a
//  preset index (0..NUM_PRESET_COLORS-1); the host resolves clashes at match
//  start (next-free-preset) into the palette every screen renders from.
// ============================================================================

#define NUM_PRESET_COLORS 5

static const CRGB PRESET_COLORS[NUM_PRESET_COLORS] = {
  CRGB(0, 200, 255),   // 0 cyan
  CRGB(255, 110, 0),   // 1 orange
  CRGB(0, 220, 60),    // 2 green
  CRGB(230, 0, 200),   // 3 magenta
  CRGB(230, 210, 0),   // 4 yellow
};
