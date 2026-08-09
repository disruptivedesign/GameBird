#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include "shapes.h"     // SHAPES, shapeCell -- the rules half of the piece data

// ============================================================================
//  Tetris-specific LOOKS: piece colors and menu icons. (Data only -- included
//  by tetris.cpp. Piece geometry lives in shapes.h so the sim can share it; the
//  generic digit font lives in Display.)
// ============================================================================

// Classic piece colors (RGB; white channel is synthesized by RgbwDefault()).
static const CRGB COLORS[TET_NUM_TYPES] = {
  CRGB(0, 255, 255),   // I cyan
  CRGB(255, 200, 0),   // O yellow
  CRGB(160, 0, 255),   // T purple
  CRGB(0, 255, 0),     // S green
  CRGB(255, 0, 0),     // Z red
  CRGB(0, 0, 255),     // J blue
  CRGB(255, 90, 0),    // L orange
};

// Dim white ghost outline. Mixed from all three channels on WS2812B -- rev 1's
// SK6812 had a dedicated white die and got this for a third of the current.
// 32 is the lowest that survives brightness scaling (scale8(32,8) == 1).
static const CRGB GHOST_COLOR = CRGB(64, 64, 64);

// Menu icons (8x8, row 0 = top; codes index ICON_PAL, 0 = off).
static const CRGB ICON_PAL[] = {
  CRGB::Black,          // 0 off
  CRGB(160, 0, 255),    // 1 purple  (tetromino)
  CRGB(230, 230, 230),  // 2 white   (ghost body)
  CRGB(0, 80, 255),     // 3 blue    (ghost eyes)
  CRGB(255, 200, 0),    // 4 yellow  (trophy)
};

static const uint8_t IMG_NORMAL[8][8] = {   // large T tetromino
  {0,0,0,0,0,0,0,0},
  {0,1,1,1,1,1,1,0},
  {0,1,1,1,1,1,1,0},
  {0,0,0,1,1,0,0,0},
  {0,0,0,1,1,0,0,0},
  {0,0,0,1,1,0,0,0},
  {0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0},
};

static const uint8_t IMG_TETRIS[8][8] = {   // large S tetromino
  {0,0,0,0,0,0,0,0},
  {0,0,0,1,1,1,1,0},
  {0,0,0,1,1,1,1,0},
  {0,1,1,1,1,0,0,0},
  {0,1,1,1,1,0,0,0},
  {0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0},
};
static const uint8_t IMG_GHOST[8][8] = {    // classic ghost sprite
  {0,0,2,2,2,2,0,0},
  {0,2,2,2,2,2,2,0},
  {2,2,3,2,2,3,2,2},
  {2,2,3,2,2,3,2,2},
  {2,2,2,2,2,2,2,2},
  {2,2,2,2,2,2,2,2},
  {2,2,2,2,2,2,2,2},
  {2,0,2,2,2,2,0,2},
};
static const uint8_t IMG_SCORE[8][8] = {    // trophy (high-score page)
  {0,4,4,4,4,4,4,0},
  {0,4,4,4,4,4,4,0},
  {0,4,4,4,4,4,4,0},
  {0,0,4,4,4,4,0,0},
  {0,0,0,4,4,0,0,0},
  {0,0,0,4,4,0,0,0},
  {0,0,4,4,4,4,0,0},
  {0,4,4,4,4,4,4,0},
};
