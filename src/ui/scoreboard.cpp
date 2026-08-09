#include "scoreboard.h"
#include "core/defines.h"

// One home for the marquee rate, so Tetris and Breakout cannot drift apart.
// The rate is per pixel of travel, so doubling the glyph size below would have
// doubled the time a score takes to cross; 70 keeps a five-digit pass at about
// the same four seconds it was at 1x.
#define SCROLL_MS    70    // ms per pixel of travel
#define SCORE_SCALE   2    // the 3x5 font is an afterthought at 1x on 16 rows
#define FONT_H        5    // unscaled glyph height

// Travel is panel width + number width + 1: the number enters from just off the
// right edge and the pass is not over until its last column has left on the far
// side, plus one blank so a looping scroll does not butt the digits together.
static int scrollTravel(Display& d, uint32_t value){
  return MATRIX_W + d.numberWidth(value, SCORE_SCALE) + 1;
}

static void drawAt(Display& d, uint32_t value, int pos, const CRGB& c){
  d.drawNumber(value, MATRIX_W - pos, (MATRIX_H - FONT_H * SCORE_SCALE) / 2, c,
               SCORE_SCALE);
}

bool drawScoreScroll(Display& d, uint32_t value, uint32_t elapsedMs, const CRGB& c){
  int pos = (int)(elapsedMs / SCROLL_MS);
  drawAt(d, value, pos, c);
  return pos > scrollTravel(d, value);
}

void drawScoreScrollLoop(Display& d, uint32_t value, uint32_t nowMs, const CRGB& c){
  int pos = (int)((nowMs / SCROLL_MS) % (uint32_t)scrollTravel(d, value));
  drawAt(d, value, pos, c);
}

void drawWinBars(Display& d, const uint8_t* scores, const CRGB* colors,
                 int n, uint8_t winner, uint32_t now){
  d.clear();
  if (n < 1) n = 1;
  bool blink = (now / 250) & 1;
  for (int i = 0; i < n; i++){
    int x = (2 * i + 1) * MATRIX_W / (2 * n);     // evenly spaced columns
    int h = scores[i];
    if (h > MATRIX_H) h = MATRIX_H;
    for (int r = 0; r < h; r++) d.setPixel(x, MATRIX_H - 1 - r, colors[i]);
    // Winner marker floats one row ABOVE the bar, never on it: a 1-tall bar is
    // a single pixel, and drawing the marker on it would hide the player's
    // color entirely (the bar would just blink white).
    if (i == winner && h > 0 && h < MATRIX_H && blink)
      d.setPixel(x, MATRIX_H - 1 - h, CRGB(255, 255, 255));
  }
}
