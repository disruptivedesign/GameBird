#pragma once
#include <stdint.h>

// ============================================================================
//  Tetromino geometry: the part of the piece data that is rules rather than
//  looks. Pure -- no <Arduino.h>, no FastLED -- so TetrisSim and the native
//  test env can include it. Colors and menu art stay in pieces.h, which the
//  renderer includes instead (and which includes this).
// ============================================================================

#define TET_NUM_TYPES 7

// 4x4 bitmaps, bit 15 = top-left, row-major.
// Order: I, O, T, S, Z, J, L -- 4 rotation states each.
static const uint16_t SHAPES[TET_NUM_TYPES][4] = {
  {0x0F00, 0x2222, 0x00F0, 0x4444}, // I
  {0x0660, 0x0660, 0x0660, 0x0660}, // O
  {0x0E40, 0x4C40, 0x4E00, 0x4640}, // T
  {0x06C0, 0x8C40, 0x06C0, 0x8C40}, // S
  {0x0C60, 0x4C80, 0x0C60, 0x4C80}, // Z
  {0x08E0, 0x6440, 0x0E20, 0x44C0}, // J
  {0x02E0, 0x4460, 0x0E80, 0xC440}, // L
};

// Extract a cell from a 4x4 shape bitmap.
inline bool shapeCell(uint16_t shape, int c, int r) {
  return (shape >> (15 - (r * 4 + c))) & 0x1;
}
