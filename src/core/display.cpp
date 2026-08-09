#include "display.h"
#include <driver/gpio.h>     // gpio_set_drive_capability

// 3x5 digit font: each row is 3 bits (bit2 = left column).
static const uint8_t DIGITS[10][5] = {
  {7,5,5,5,7}, // 0
  {2,6,2,2,7}, // 1
  {7,1,7,4,7}, // 2
  {7,1,7,1,7}, // 3
  {5,5,7,1,1}, // 4
  {7,4,7,1,7}, // 5
  {7,4,7,5,7}, // 6
  {7,1,2,2,2}, // 7
  {7,5,7,5,7}, // 8
  {7,5,7,1,7}, // 9
};

// 3x5 uppercase font, A-Z (same 3-bit-per-row encoding).
static const uint8_t LETTERS[26][5] = {
  {2,5,7,5,5}, // A
  {6,5,6,5,6}, // B
  {3,4,4,4,3}, // C
  {6,5,5,5,6}, // D
  {7,4,6,4,7}, // E
  {7,4,6,4,4}, // F
  {3,4,5,5,3}, // G
  {5,5,7,5,5}, // H
  {7,2,2,2,7}, // I
  {1,1,1,5,2}, // J
  {5,6,4,6,5}, // K
  {4,4,4,4,7}, // L
  {5,7,5,5,5}, // M
  {5,7,7,5,5}, // N
  {2,5,5,5,2}, // O
  {6,5,6,4,4}, // P
  {2,5,5,6,3}, // Q
  {6,5,6,5,5}, // R
  {3,4,2,1,6}, // S
  {7,2,2,2,2}, // T
  {5,5,5,5,7}, // U
  {5,5,5,5,2}, // V
  {5,5,5,7,5}, // W
  {5,5,2,5,5}, // X
  {5,5,2,2,2}, // Y
  {7,1,2,4,7}, // Z
};

void Display::begin(){
  // WS2812B: 3 bytes per pixel, no white die. Rev 1's SK6812 needed
  // .setRgbw(RgbwDefault()) and a 4/3-oversized buffer; neither applies here,
  // and the buffer is exactly one CRGB per LED again.
  FastLED.addLeds<WS2812B, DISP_LED_PIN, GRB>(_leds, NUM_LEDS_DATA);

  // Strongest available drive capability, after addLeds has claimed the pin
  // as an output. Sharper edges resist noise pickup on the data line better
  // than the default (~medium) strength -- see the FASTLED_OVERCLOCK_WS2812
  // note in defines.h for why that matters on this protocol.
  gpio_set_drive_capability((gpio_num_t)DISP_LED_PIN, GPIO_DRIVE_CAP_3);

  // Without a dedicated white die, neutrals are mixed from three channels and
  // come out tinted uncorrected. Rev 1 largely masked this.
  FastLED.setCorrection(TypicalLEDStrip);

  // Before setBrightness, so the very first frame is already inside the rail.
  // FastLED re-scales per frame to hold this ceiling -- see DISPLAY_MAX_MA.
  FastLED.setMaxPowerInVoltsAndMilliamps(5, DISPLAY_MAX_MA);

  FastLED.setBrightness(DISPLAY_BRIGHTNESS);

  // Off, not the default. Temporal dithering fakes extra bit depth by
  // alternating a pixel between two output levels across frames, which reads
  // as noise rather than resolution on dim, mostly-static UI colours -- see
  // DISPLAY_BRIGHTNESS. Nothing here needs the smoother gradient it buys.
  FastLED.setDither(DISABLE_DITHER);

  clear();
  show();
}

void Display::setBrightness(uint8_t b){ FastLED.setBrightness(b); }
void Display::clear(){ fill_solid(_leds, NUM_LEDS_DATA, CRGB::Black); }
void Display::show(){ FastLED.show(); }
void Display::fill(const CRGB& c){ fill_solid(_leds, NUM_LEDS_DATA, c); }

// Map a logical (col,row) to a data index, or -1 for a bypassed pixel.
//
// Tiling: the chain fills one module completely before reaching the next, so a
// panel built from smaller modules needs the module's base offset added before
// the within-module raster. With PANEL_TILE_* equal to the panel this is the
// identity -- tx, ty and tileBase are all zero and the expression collapses to
// the plain y * MATRIX_W + x it was.
int Display::cellToData(int col, int row) const {
  int x = col, y = row;
  if (TRANSPOSE){ int t = x; x = y; y = t; }
  if (FLIP_X) x = MATRIX_W - 1 - x;
  if (FLIP_Y) y = MATRIX_H - 1 - y;

  const int tilesX   = MATRIX_W / PANEL_TILE_W;
  const int tileBase = ((y / PANEL_TILE_H) * tilesX + (x / PANEL_TILE_W))
                       * PANEL_TILE_W * PANEL_TILE_H;
  const int lx = x % PANEL_TILE_W;
  const int ly = y % PANEL_TILE_H;

  // Serpentine is a property of how a MODULE is wired, so it alternates within
  // the tile rather than across the whole panel.
  int p;
  if (MATRIX_SERPENTINE && (ly & 1)) p = tileBase + ly * PANEL_TILE_W + (PANEL_TILE_W - 1 - lx);
  else                               p = tileBase + ly * PANEL_TILE_W + lx;

  if (DEAD_INDEX >= 0){
    if (p == DEAD_INDEX) return -1;    // permanently dark cell
    if (p > DEAD_INDEX)  p--;          // downstream shift from the bypass
  }
  return p;
}

void Display::setPixel(int col, int row, const CRGB& c){
  if (col < 0 || col >= MATRIX_W || row < 0 || row >= MATRIX_H) return;  // off-screen
  int idx = cellToData(col, row);
  if (idx >= 0 && idx < NUM_LEDS_DATA) _leds[idx] = c;
}

void Display::border(const CRGB& c){
  for (int x = 0; x < MATRIX_W; x++){ setPixel(x, 0, c); setPixel(x, MATRIX_H - 1, c); }
  for (int y = 0; y < MATRIX_H; y++){ setPixel(0, y, c); setPixel(MATRIX_W - 1, y, c); }
}

// ============================================================================
//  Text
// ============================================================================
int Display::digitCount(uint32_t v) const { int n = 1; while (v >= 10){ v /= 10; n++; } return n; }

// 3px glyph + 1px gap, both scaled, with no trailing gap.
int Display::numberWidth(uint32_t v, int scale) const {
  if (scale < 1) scale = 1;
  return digitCount(v) * 4 * scale - scale;
}

int Display::textWidth(const char* s, int scale) const {
  if (scale < 1) scale = 1;
  int n = 0; while (s[n]) n++;
  return n > 0 ? n * 4 * scale - scale : 0;
}

// One glyph of the 3x5 font, every source pixel drawn as a scale x scale block.
void Display::drawGlyph(const uint8_t* g, int x0, int topRow, const CRGB& col, int scale){
  for (int row = 0; row < 5; row++)
    for (int c = 0; c < 3; c++){
      if (!(g[row] & (0x4 >> c))) continue;
      for (int dy = 0; dy < scale; dy++)
        for (int dx = 0; dx < scale; dx++)
          setPixel(x0 + c * scale + dx, topRow + row * scale + dy, col);
    }
}

void Display::drawNumber(uint32_t v, int x0, int topRow, const CRGB& col, int scale){
  if (scale < 1) scale = 1;
  int n = digitCount(v);
  char buf[12];
  for (int i = n - 1; i >= 0; i--){ buf[i] = (char)('0' + (v % 10)); v /= 10; }
  int x = x0;
  for (int i = 0; i < n; i++){
    drawGlyph(DIGITS[buf[i] - '0'], x, topRow, col, scale);
    x += 4 * scale;
  }
}

// Draw a single glyph (0-9, A-Z; anything else renders blank).
void Display::drawChar(char ch, int x0, int topRow, const CRGB& col, int scale){
  if (scale < 1) scale = 1;
  const uint8_t* g = nullptr;
  if (ch >= '0' && ch <= '9')      g = DIGITS[ch - '0'];
  else if (ch >= 'A' && ch <= 'Z') g = LETTERS[ch - 'A'];
  else return;
  drawGlyph(g, x0, topRow, col, scale);
}

void Display::drawText(const char* s, int x0, int topRow, const CRGB& col, int scale){
  if (scale < 1) scale = 1;
  int x = x0;
  for (int i = 0; s[i]; i++){ drawChar(s[i], x, topRow, col, scale); x += 4 * scale; }
}

// ============================================================================
//  Icon art
// ============================================================================
// Icons are authored at ICON_W x ICON_H whatever the panel is (see defines.h),
// so scale up by the largest whole factor that fits. An 8x8 panel gets factor 1
// -- byte-for-byte what this did before -- and a 16x16 panel draws every source
// pixel as a 2x2 block and fills the screen. Nearest-neighbour on purpose: at
// eight pixels across, every icon is hand-placed pixel art, and interpolating
// would only blur the edges that carry the shape.
int Display::iconScale() const {
  int s = (MATRIX_W / ICON_W) < (MATRIX_H / ICON_H) ? (MATRIX_W / ICON_W)
                                                    : (MATRIX_H / ICON_H);
  return s < 1 ? 1 : s;            // panel smaller than the art: clip
}

void Display::drawBitmapAt(const uint8_t img[ICON_H][ICON_W], const CRGB* pal,
                           int scale, int x0, int y0){
  if (scale < 1) scale = 1;
  for (int r = 0; r < ICON_H; r++)
    for (int c = 0; c < ICON_W; c++){
      uint8_t code = img[r][c];
      if (!code) continue;                // 0 = transparent, not black
      const CRGB& col = pal[code];
      for (int dy = 0; dy < scale; dy++)
        for (int dx = 0; dx < scale; dx++)
          setPixel(x0 + c * scale + dx, y0 + r * scale + dy, col);
    }
}

void Display::drawBitmap(const uint8_t img[ICON_H][ICON_W], const CRGB* pal){
  int s = iconScale();
  drawBitmapAt(img, pal, s, (MATRIX_W - ICON_W * s) / 2, (MATRIX_H - ICON_H * s) / 2);
}

// Confirms a mapping once you have one: row 0 green, column 0 red, origin white.
// It goes THROUGH cellToData, so it validates the settings rather than
// discovering them -- if the panel is serpentine or tiled and you have not said
// so, solid lines still come out looking like solid lines.
void Display::calibrate(){
  clear();
  for (int c = 0; c < MATRIX_W; c++) setPixel(c, 0, CRGB::Green);   // row 0  -> green line
  for (int r = 0; r < MATRIX_H; r++) setPixel(0, r, CRGB::Red);     // col 0  -> red line
  setPixel(0, 0, CRGB::White);                                      // origin -> white
  show();
}

// Discovers a mapping from scratch. Paints the chain in DATA order, bypassing
// cellToData and therefore every orientation and tiling setting: hue ramps from
// red at the first LED the data line reaches, round to red again at the last,
// and every MATRIX_W-th pixel along the chain is white.
//
// One look answers all of it:
//   - the red end is where the data line enters, so that corner is index 0
//   - follow the hue to see which way rows run, and whether they alternate
//     (serpentine) or all start from the same side (progressive)
//   - the white marks are one panel-width apart ALONG THE CHAIN. On a
//     continuous panel they line up in a straight column. If instead they land
//     halfway across, the modules are narrower than the panel and PANEL_TILE_W
//     is what needs setting.
void Display::calibrateChain(){
  for (int i = 0; i < NUM_LEDS_DATA; i++){
    const uint8_t hue = (uint8_t)((long)i * 255 / NUM_LEDS_DATA);
    _leds[i] = (i % MATRIX_W == 0) ? CRGB(255, 255, 255)
                                   : (CRGB)CHSV(hue, 255, 200);
  }
  show();
}
