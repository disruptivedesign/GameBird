#include "swarm_app.h"
#include "waves.h"
#include "core/palette.h"
#include "ui/widgets.h"
#include "audio/sfx.h"

#define SWMAPP_PAGES  2      // Single Player, Multiplayer
#define LABEL_SCALE   2      // the 3x5 font is lost at 1x on a 16-tall panel

// ============================================================================
//  The five loadouts, drawn as what they DO rather than named.
//
//  A three-letter label would not survive this panel, and more to the point a
//  name does not tell you anything: "Spread" and "Blaster" are equally
//  meaningless until you have seen the shape each one puts on the grid. So the
//  picker shows that shape -- a single bolt, a full column, a blast, a fan, a
//  bar -- above the ship that fires it. Code 1 is the weapon and takes the
//  player's own colour; code 2 is the ship.
// ============================================================================
static const uint8_t WPN_GLYPH[SWM_W_COUNT][5][5] = {
  { {0,0,1,0,0},          // BLASTER -- one bolt, and another behind it
    {0,0,0,0,0},
    {0,0,1,0,0},
    {0,0,0,0,0},
    {0,2,2,2,0} },
  { {0,0,1,0,0},          // LASER -- the whole column
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,2,2,2,0} },
  { {0,1,1,1,0},          // BOMB -- a blast, not a bolt
    {0,1,0,1,0},
    {0,1,1,1,0},
    {0,0,0,0,0},
    {0,2,2,2,0} },
  { {1,0,1,0,1},          // SPREAD -- a fan
    {0,1,1,1,0},
    {0,0,1,0,0},
    {0,0,0,0,0},
    {0,2,2,2,0} },
  { {0,0,1,0,0},          // SHIELD -- a bar to stand behind, and what it becomes
    {0,0,0,0,0},
    {1,1,1,1,1},
    {0,0,0,0,0},
    {0,2,2,2,0} },
};

void SwarmApp::begin(){
  _state = SUB;
  _sel = 0;
  _wantExit = false;
  // The loadout is a device preference, remembered like the colour preset. A
  // player who always flies the Shield should never have to pick it twice.
  _game.setLocalWeapon((uint8_t)_sys.storage.getU32(SWM_NS, SWM_KEY_WPN, SWM_W_BLASTER));
  _best = (uint8_t)_sys.storage.getU32(SWM_NS, SWM_KEY_BEST, 0);
  _recorded = false;
}

// A forced quit from inside the submenu arrives at SwarmApp, not at the child
// actually holding the radio, so the teardown has to be handed down.
void SwarmApp::end(){
  if      (_state == MULTI)  _mp.end();
  else if (_state == SINGLE) _sp.end();
  _state = SUB;
  _sel = 0;
  _wantExit = false;
}

GameStatus SwarmApp::service(){
  switch (_state){
    case SUB: serviceSub(); break;
    case SINGLE:
      if (_sp.service() == GAME_EXIT){ _state = SUB; _sel = 0; }
      else recordBest();
      break;
    case MULTI:
      if (_mp.service() == GAME_EXIT){ _state = SUB; _sel = 1; }
      else recordBest();
      break;
  }
  if (_wantExit){ _wantExit = false; return GAME_EXIT; }
  return GAME_CONTINUE;
}

// Polled while a match runs rather than hooked to its end, because the runners
// own the end of a match and neither offers a "this one is finished" callback.
// The sim's own phase is the signal, and the latch is what stops a run being
// written once per frame for the three seconds the result screen is up.
void SwarmApp::recordBest(){
  const SwarmSim& s = _game.sim();
  if (!s.over()){ _recorded = false; return; }
  if (_recorded) return;
  _recorded = true;

  // A cleared run counts as one past the last wave, so beating the boss is
  // visibly further than dying on it.
  const uint8_t reached = s.won() ? (uint8_t)(SWM_WAVES + 1) : s.wave();
  if (reached > _best){
    _best = reached;
    _sys.storage.putU32(SWM_NS, SWM_KEY_BEST, _best);
  }
}

void SwarmApp::serviceSub(){
  if (int8_t s = _sys.joy.stepX()){
    _sel = (_sel + s + SWMAPP_PAGES) % SWMAPP_PAGES;
    _sys.audio.play(SFX_MOVE);
  }

  // +y is up on the stick, and stepping "up" through a list should move
  // forward through it -- same convention every selector on the device uses.
  if (int8_t s = _sys.joy.stepY()){
    uint8_t w = (uint8_t)((_game.localWeapon() + SWM_W_COUNT + s) % SWM_W_COUNT);
    _game.setLocalWeapon(w);
    _sys.storage.putU32(SWM_NS, SWM_KEY_WPN, w);
    _sys.audio.play(SFX_MOVE);
  }

  if (_sys.buttonB.wasPressed()){ _sys.audio.play(SFX_BACK); _wantExit = true; return; }
  if (_sys.buttonA.wasPressed()){
    _sys.audio.play(SFX_SELECT);
    if (_sel == 0){ _sp.begin(); _state = SINGLE; }
    else          { _mp.begin(); _state = MULTI; }
    return;
  }

  disp().clear();
  drawWeapon((MATRIX_W - 5) / 2, 0);

  // The furthest this unit has ever got, top right, in the same dim blue the
  // in-game HUD uses for a wave already behind you -- so the number reads as
  // "waves" without needing a label there is no room for.
  if (_best){
    const int w = disp().numberWidth(_best, 1);
    disp().drawNumber(_best, MATRIX_W - w, 0, CRGB(0, 70, 100), 1);
  }

  const char* label = (_sel == 0) ? "1P" : "MP";
  const int   w = disp().textWidth(label, LABEL_SCALE);
  disp().drawText(label, (MATRIX_W - w) / 2, 5,
                  PRESET_COLORS[_sys.net.myColorIndex() % NUM_PRESET_COLORS], LABEL_SCALE);
  drawSelectorDots(disp(), MATRIX_H - 1, SWMAPP_PAGES, _sel);
  disp().show();
}

void SwarmApp::drawWeapon(int x0, int row) const {
  const uint8_t w = _game.localWeapon() % SWM_W_COUNT;
  const CRGB mine = PRESET_COLORS[_sys.net.myColorIndex() % NUM_PRESET_COLORS];
  for (int y = 0; y < 5; y++)
    for (int x = 0; x < 5; x++){
      const uint8_t code = WPN_GLYPH[w][y][x];
      if (!code) continue;
      _sys.display.setPixel(x0 + x, row + y, code == 1 ? mine : CRGB(90, 90, 100));
    }
}
