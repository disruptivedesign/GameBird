#pragma once
#include <FastLED.h>
#include "defines.h"

// ============================================================================
//  Display: owns the LED buffer and hides every panel quirk (WS2812B RGB,
//  orientation, dead-pixel remap). Games draw in clean (col,row) space, with
//  row 0 at the TOP and rows growing downward.
// ============================================================================
class Display {
public:
  void begin();
  void setBrightness(uint8_t b);

  void clear();
  void show();
  void fill(const CRGB& c);
  void setPixel(int col, int row, const CRGB& c);
  void border(const CRGB& c);

  // ---- Text (3x5 font: digits 0-9 and uppercase A-Z; other chars blank) ----
  // Every entry point takes a whole-number `scale`. At 16x16 the 1x font is
  // small enough to be an afterthought, so most callers want 2 -- but scale is
  // explicit rather than defaulted-by-panel, because a HUD squeezed beside a
  // playfield still wants 1x and should say so.
  int  digitCount(uint32_t v) const;
  int  numberWidth(uint32_t v, int scale = 1) const;             // pixel width
  int  textWidth(const char* s, int scale = 1) const;            // pixel width
  void drawNumber(uint32_t v, int x0, int topRow, const CRGB& c, int scale = 1);
  void drawChar(char ch, int x0, int topRow, const CRGB& c, int scale = 1);
  void drawText(const char* s, int x0, int topRow, const CRGB& c, int scale = 1);

  // ---- Icon-sized art (ICON_W x ICON_H). Codes index pal[]; 0 = transparent.
  // drawBitmap scales by the largest whole factor that fits and centers it.
  // drawBitmapAt places it explicitly, which is what a sliding menu transition
  // needs -- setPixel clips, so drawing partly off-panel is safe.
  void drawBitmap(const uint8_t img[ICON_H][ICON_W], const CRGB* pal);
  void drawBitmapAt(const uint8_t img[ICON_H][ICON_W], const CRGB* pal,
                    int scale, int x0, int y0);
  int  iconScale() const;      // the whole factor drawBitmap would pick

  void calibrate();        // orientation test pattern -- confirms a mapping
  void calibrateChain();   // raw chain in data order -- discovers one

  int  count() const { return NUM_LEDS_DATA; }

private:
  int  cellToData(int col, int row) const;
  void drawGlyph(const uint8_t* g, int x0, int topRow, const CRGB& c, int scale);
  CRGB _leds[NUM_LEDS_DATA];
};
