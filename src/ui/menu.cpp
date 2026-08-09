#include "menu.h"
#include "audio/sfx.h"

#define SLIDE_MS       120    // icon transition length
#define STRIP_HOLD_MS  500    // position strip stays lit this long after a step
#define STRIP_FADE_MS  400    // then fades out over this

// Returning from a game keeps _page, so re-entry lands where you left. Flash the
// strip on the way in, though: after a couple of minutes inside Tetris, "which
// entry am I on" is exactly the question, and answering it costs one row for
// half a second.
void Menu::begin(){
  _slideDir = 0;
  _strokeAt = millis();
}

Icon Menu::iconFor(int i){
  return _entries[i].game ? _entries[i].game->menuIcon() : _entries[i].icon;
}

Game* Menu::service(){
  uint32_t now = millis();

  int8_t s = _sys.joy.stepX();
  if (s){
    _prevPage   = _page;
    _page       = (_page + s + _count) % _count;   // wraps, which a slider could not
    _slideDir   = s;
    _slideStart = now;
    _strokeAt   = now;
    _sys.audio.play(SFX_MOVE);
  }

  if (_sys.buttonA.wasPressed() || _sys.buttonB.wasPressed()){
    if (_entries[_page].game){
      _sys.audio.play(SFX_SELECT);
      return _entries[_page].game;                 // launch it
    }
    // non-launchable (placeholder): ignore the press
  }

  render(now);
  return nullptr;
}

// N pixels along the bottom row, evenly spaced, current one white. Drawn over
// the icon rather than beside it -- a 2x icon fills all 16 rows and there is no
// margin to give it -- which is fine precisely because it goes away again.
void Menu::drawStrip(uint32_t now){
  uint32_t age = now - _strokeAt;
  if (age >= STRIP_HOLD_MS + STRIP_FADE_MS) return;

  uint8_t k = 255;
  if (age > STRIP_HOLD_MS)
    k = (uint8_t)(255 - (age - STRIP_HOLD_MS) * 255 / STRIP_FADE_MS);

  const int row = MATRIX_H - 1;
  for (int i = 0; i < _count; i++){
    int x = (2 * i + 1) * MATRIX_W / (2 * _count);
    // White for the current entry, a dim grey for the rest, both faded together.
    uint8_t v = (i == _page) ? k : (uint8_t)(k / 6);
    _sys.display.setPixel(x, row, CRGB(v, v, v));
  }
}

void Menu::render(uint32_t now){
  _sys.display.clear();

  const int s  = _sys.display.iconScale();
  const int y0 = (MATRIX_H - ICON_H * s) / 2;

  uint32_t t = now - _slideStart;
  if (_slideDir && t < SLIDE_MS){
    // Both icons travel one full panel width: the new one in from the side you
    // stepped toward, the old one out the other way.
    int off = (int)((uint32_t)MATRIX_W * t / SLIDE_MS);
    Icon in  = iconFor(_page);
    Icon out = iconFor(_prevPage);
    _sys.display.drawBitmapAt(in.bmp,  in.pal,  s,  _slideDir * (MATRIX_W - off), y0);
    _sys.display.drawBitmapAt(out.bmp, out.pal, s, -_slideDir * off,              y0);
  } else {
    _slideDir = 0;
    Icon ic = iconFor(_page);
    _sys.display.drawBitmapAt(ic.bmp, ic.pal, s, (MATRIX_W - ICON_W * s) / 2, y0);
  }

  drawStrip(now);
  _sys.display.show();
}
