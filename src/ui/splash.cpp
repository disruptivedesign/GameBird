#include "splash.h"
#include "audio/sfx.h"

#define WIPE_MS      1000    // rainbow sweep duration
#define WIPE_BAND       5    // columns lit behind the leading edge
#define TITLE        "GAMEBIRD"
#define TITLE_SCALE     2    // the 3x5 font is lost at 1x on a 16-tall panel
#define TITLE_SCROLL   45    // ms per pixel of travel

// Phase 1: a rainbow band sweeping left to right.
//
// A BAND rather than a cumulative fill, and that is a power decision, not a
// visual one. This is the first thing that runs after net.begin(), so the radio
// is already up, and a full panel of saturated colour would make the splash the
// highest-current frame the device ever draws -- at boot, stacked on top of
// power-on inrush. Five lit columns is ~80 pixels instead of 256. It also
// happens to read better: the fade behind the leading edge gives the sweep a
// direction that a fill does not have.
static void wipe(Display& d, Audio& a){
  uint32_t start = millis();
  for (;;){
    uint32_t now = millis();
    a.update(now);                       // nothing else is pumping audio at boot
    uint32_t t = now - start;
    if (t >= WIPE_MS) break;

    d.clear();
    // The edge travels one band past the right side so the tail clears too.
    int lead = (int)(t * (uint32_t)(MATRIX_W + WIPE_BAND) / WIPE_MS);
    for (int k = 0; k < WIPE_BAND; k++){
      int c = lead - k;
      if (c < 0 || c >= MATRIX_W) continue;
      uint8_t v = (uint8_t)(255 - (k * 255) / WIPE_BAND);
      for (int r = 0; r < MATRIX_H; r++)
        d.setPixel(c, r, CHSV((uint8_t)(c * 14 + r * 8), 255, v));
    }
    d.show();
    delay(5);
  }
}

// Phase 2: scroll "GAMEBIRD" right-to-left, each letter a flowing rainbow hue.
static void scrollTitle(Display& d, Audio& a){
  const int textW = d.textWidth(TITLE, TITLE_SCALE);
  const int row   = (MATRIX_H - 5 * TITLE_SCALE) / 2;
  const int step  = 4 * TITLE_SCALE;
  const int travel = MATRIX_W + textW;

  uint32_t start = millis();
  for (;;){
    uint32_t now = millis();
    a.update(now);
    int pos = (int)((now - start) / TITLE_SCROLL);
    if (pos > travel) break;

    d.clear();
    int x = MATRIX_W - pos;
    for (int i = 0; TITLE[i]; i++){
      uint8_t hue = (uint8_t)(now / 6 + x * 10);   // flows with time + position
      d.drawChar(TITLE[i], x, row, CHSV(hue, 255, 255), TITLE_SCALE);
      x += step;
    }
    d.show();
    delay(5);
  }
}

void Splash::play(){
  _a.play(SFX_BOOT);
  wipe(_d, _a);
  scrollTitle(_d, _a);
  _d.clear();
  _d.show();
}
