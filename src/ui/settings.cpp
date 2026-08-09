#include "settings.h"
#include "core/palette.h"
#include "widgets.h"
#include "audio/sfx.h"

#define SETTINGS_PAGES  2      // User Color, Volume
#define VOL_LEVELS      (VOL_HIGH + 1)

#define ROW_VALUE  (MATRIX_H - 2)    // value dots
#define ROW_PAGE   (MATRIX_H - 1)    // which page you are on

// ---- menu icon: a gear / cog ----
static const uint8_t IMG_GEAR[8][8] = {
  {0,0,1,0,0,1,0,0},
  {1,0,1,1,1,1,0,1},
  {0,1,1,1,1,1,1,0},
  {1,1,1,0,0,1,1,1},
  {1,1,1,0,0,1,1,1},
  {0,1,1,1,1,1,1,0},
  {1,0,1,1,1,1,0,1},
  {0,0,1,0,0,1,0,0},
};
static const CRGB GEAR_PAL[] = { CRGB::Black, CRGB(170, 170, 170) };

Icon Settings::menuIcon() const { return { IMG_GEAR, GEAR_PAL }; }

void Settings::begin(){
  _page        = 0;
  _colorSel    = _colorBefore = _sys.net.myColorIndex();
  _volSel      = _volBefore   = _sys.audio.volume();
  _wantExit    = false;
}

// ============================================================================
//  Input
// ============================================================================
// Up increases, down decreases -- so `step` arrives already flipped into screen
// space by Joystick::stepY() and has to be negated to read as "more".
void Settings::adjust(int8_t step){
  int delta = -step;
  if (_page == 0){
    _colorSel = (uint8_t)((_colorSel + delta + NUM_PRESET_COLORS) % NUM_PRESET_COLORS);
    _sys.audio.play(SFX_MOVE);
  } else {
    _volSel = (uint8_t)((_volSel + delta + VOL_LEVELS) % VOL_LEVELS);
    // Apply immediately, unpersisted. The blip that follows is the preview, and
    // it plays AT the new level -- which is the only honest way to audition a
    // volume control.
    _sys.audio.setVolume((Volume)_volSel, false);
    _sys.audio.play(SFX_SELECT);
  }
}

void Settings::commit(){
  if (_colorSel != _colorBefore) _sys.net.setColorIndex(_colorSel);
  _sys.audio.setVolume((Volume)_volSel, true);      // persists to NVS
  _sys.audio.play(SFX_SELECT);
}

void Settings::revert(){
  _sys.audio.setVolume(_volBefore, false);          // undo the previews
  _colorSel = _colorBefore;                         // never left this screen
  _sys.audio.play(SFX_BACK);
}

GameStatus Settings::service(){
  if (_sys.buttonA.wasPressed()){ commit(); _wantExit = true; }
  else if (_sys.buttonB.wasPressed()){ revert(); _wantExit = true; }
  else {
    if (int8_t sx = _sys.joy.stepX()){
      _page = (uint8_t)((_page + sx + SETTINGS_PAGES) % SETTINGS_PAGES);
      _sys.audio.play(SFX_MOVE);
    }
    if (int8_t sy = _sys.joy.stepY()) adjust(sy);
    render();
  }

  if (_wantExit){ _wantExit = false; return GAME_EXIT; }
  return GAME_CONTINUE;
}

// ============================================================================
//  Rendering
// ============================================================================
void Settings::render(){
  disp().clear();
  if (_page == 0) drawColorPage();
  else            drawVolumePage();

  drawSelectorDots(disp(), ROW_PAGE, SETTINGS_PAGES, _page);
  disp().show();
}

// A large centred swatch rather than a full-screen fill. Partly so the value
// dots and page strip have somewhere to live, and partly for the rail: a
// full-panel saturated colour is a lot of lit channels for a screen you sit on
// while deciding.
void Settings::drawColorPage(){
  const CRGB c = PRESET_COLORS[_colorSel];
  const int  m = 3;
  for (int y = m; y < ROW_VALUE - 1; y++)
    for (int x = m; x < MATRIX_W - m; x++) disp().setPixel(x, y, c);

  drawSelectorDots(disp(), ROW_VALUE, NUM_PRESET_COLORS, _colorSel);
}

void Settings::drawVolumePage(){
  drawVolumeMeter(_volSel);
  drawSelectorDots(disp(), ROW_VALUE, VOL_LEVELS, _volSel);
}

// Three bars, growing left to right, lit up to `lvl` (1..3). Level 0 is muted:
// every bar dark plus a red corner pip, so "off" looks deliberate rather than
// like a panel that failed to draw.
//
// Sized from the panel rather than from 8: at 16 wide the bars are three
// columns each, and they stop short of the two indicator rows.
void Settings::drawVolumeMeter(uint8_t lvl){
  const int base = ROW_VALUE - 2;              // bottom row of the bars
  const int bw   = MATRIX_W / 5;               // 3 bars + gaps across the panel
  for (int i = 0; i < 3; i++){
    const int  x = 1 + i * (bw + 1);
    const int  h = (base + 1) * (i + 1) / 3;
    const CRGB c = (i < lvl) ? CRGB(0, 190, 0) : CRGB(18, 18, 18);
    for (int k = 0; k < h; k++)
      for (int w = 0; w < bw; w++) disp().setPixel(x + w, base - k, c);
  }
  if (lvl == VOL_OFF) disp().setPixel(MATRIX_W - 1, 0, CRGB(160, 0, 0));
}
