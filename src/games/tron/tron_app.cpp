#include "tron_app.h"
#include "core/palette.h"
#include "ui/widgets.h"
#include "audio/sfx.h"

#define TRONAPP_PAGES  2      // Single Player, Multiplayer
#define LABEL_SCALE    2      // the 3x5 font is lost at 1x on a 16-tall panel

void TronApp::begin(){
  _state = SUB;
  _sel = 0;
  _wantExit = false;
}

// A forced quit from inside the submenu arrives at TronApp, not at the child
// actually holding the radio, so the teardown has to be handed down.
void TronApp::end(){
  if      (_state == MULTI)  _mp.end();
  else if (_state == SINGLE) _sp.end();
  _state = SUB;
  _sel = 0;
  _wantExit = false;
}

GameStatus TronApp::service(){
  switch (_state){
    case SUB: serviceSub(); break;
    case SINGLE:
      if (_sp.service() == GAME_EXIT){ _state = SUB; _sel = 0; }   // quit 1P
      break;
    case MULTI:
      if (_mp.service() == GAME_EXIT){ _state = SUB; _sel = 1; }   // browse backed out
      break;
  }
  if (_wantExit){ _wantExit = false; return GAME_EXIT; }
  return GAME_CONTINUE;
}

// Submenu: left/right picks Single / Multiplayer, A enters, B backs to main menu.
void TronApp::serviceSub(){
  if (int8_t s = _sys.joy.stepX()){
    _sel = (_sel + s + TRONAPP_PAGES) % TRONAPP_PAGES;
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
  const char* label = (_sel == 0) ? "1P" : "MP";
  const int   w = disp().textWidth(label, LABEL_SCALE);
  disp().drawText(label, (MATRIX_W - w) / 2, (MATRIX_H - 5 * LABEL_SCALE) / 2 - 1,
                  PRESET_COLORS[_sys.net.myColorIndex()], LABEL_SCALE);
  drawSelectorDots(disp(), MATRIX_H - 1, TRONAPP_PAGES, _sel);
  disp().show();
}
