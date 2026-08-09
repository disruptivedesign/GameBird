#pragma once
#include <Arduino.h>
#include <FastLED.h>

// ============================================================================
//  Breakout-specific assets: brick row colours and the menu icon.
//  (Data only -- included by breakout.cpp, as pieces.h is by tetris.cpp.)
// ============================================================================

// Indexed by BreakoutSim::brickTier(): 0 = bottom row, hotter as you go up.
// Atari's ladder was yellow / green / orange / red bottom-to-top and the point
// values climbed with it, so colour reads as "worth more, and further away".
// An 8x8 shows the bottom two; a 16x16 shows all four.
#define NUM_BRICK_TIERS 4
static const CRGB BRICK_COLORS[NUM_BRICK_TIERS] = {
  CRGB(230, 210, 0),    // 0 yellow  1 pt
  CRGB(0, 220, 60),     // 1 green   3 pt
  CRGB(255, 110, 0),    // 2 orange  5 pt
  CRGB(255, 0, 0),      // 3 red     7 pt
};
inline const CRGB& brickColor(uint8_t tier) {
  return BRICK_COLORS[tier < NUM_BRICK_TIERS ? tier : NUM_BRICK_TIERS - 1];
}

static const CRGB PADDLE_COLOR = CRGB(0, 200, 255);
static const CRGB BALL_COLOR   = CRGB(230, 230, 230);

// Was CRGB(0, 200, 255) -- identical to PADDLE_COLOR. Harmless while lives had
// their own row, but now that they're pixels within the paddle itself (see
// Breakout::renderBoard), it has to read as a distinct color or the gauge is
// invisible.
static const CRGB LIFE_COLOR   = CRGB(255, 0, 180);

// Menu icon (8x8, row 0 = top; codes index ICON_PAL, 0 = off).
static const CRGB ICON_PAL[] = {
  CRGB::Black,          // 0 off
  CRGB(255, 0, 0),      // 1 red brick row
  CRGB(255, 110, 0),    // 2 orange brick row
  CRGB(0, 220, 60),     // 3 green brick row
  CRGB(230, 230, 230),  // 4 ball
  CRGB(0, 200, 255),    // 5 paddle
  CRGB(255, 200, 0),    // 6 trophy (high-score page)
};

static const uint8_t IMG_BREAKOUT[8][8] = {
  {1,1,1,1,1,1,1,1},
  {2,2,2,2,2,2,2,2},
  {3,3,3,3,3,3,3,3},
  {0,0,0,0,0,0,0,0},
  {0,0,0,0,0,4,0,0},
  {0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0},
  {0,0,5,5,5,0,0,0},
};

static const uint8_t IMG_SCORE[8][8] = {    // trophy (high-score page)
  {0,6,6,6,6,6,6,0},
  {0,6,6,6,6,6,6,0},
  {0,6,6,6,6,6,6,0},
  {0,0,6,6,6,6,0,0},
  {0,0,0,6,6,0,0,0},
  {0,0,0,6,6,0,0,0},
  {0,0,6,6,6,6,0,0},
  {0,6,6,6,6,6,6,0},
};
