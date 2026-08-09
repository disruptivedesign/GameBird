#include "virus_menu.h"
#include "virus_rules.h"
#include "core/palette.h"
#include "ui/widgets.h"
#include "audio/sfx.h"

#define VIRUSMENU_PAGES 2      // Single, Multiplayer
#define VIRUS_CHOICES   4      // A, B, C, D
#define LABEL_SCALE     2      // the 3x5 font is lost at 1x on a 16-tall panel

void VirusMenu::begin() {
  _state = SUB;
  _sel = 0;
  _wantExit = false;
}

// A forced quit lands on VirusMenu, not on the child actually holding the
// radio, so the teardown has to be handed down.
void VirusMenu::end() {
  if (_state == MULTI) _mp.end();
  _state = SUB;
  _sel = 0;
  _wantExit = false;
}

GameStatus VirusMenu::service() {
  switch (_state) {
    case SUB:  serviceSub();  break;
    case PICK: servicePick(); break;
    case SINGLE:
      if (_single.service() == GAME_EXIT) { _state = SUB; _sel = 0; }
      break;
    case MULTI:
      if (_mp.service() == GAME_EXIT) { _state = SUB; _sel = 1; }
      break;
  }
  if (_wantExit) { _wantExit = false; return GAME_EXIT; }
  return GAME_CONTINUE;
}

// Submenu: left/right picks Single / Multiplayer, A enters, B backs out.
void VirusMenu::serviceSub() {
  if (int8_t s = _sys.joy.stepX()) {
    _sel = (_sel + s + VIRUSMENU_PAGES) % VIRUSMENU_PAGES;
    _sys.audio.play(SFX_MOVE);
  }

  if (_sys.buttonB.wasPressed()) { _sys.audio.play(SFX_BACK); _wantExit = true; return; }
  if (_sys.buttonA.wasPressed()) {
    _sys.audio.play(SFX_SELECT);
    if (_sel == 0) { _single.begin(); _state = SINGLE; }
    else           { _state = PICK; }     // choose a virus before the lobby
    return;
  }

  disp().clear();
  const char* label = (_sel == 0) ? "1P" : "MP";
  const int   w = disp().textWidth(label, LABEL_SCALE);
  disp().drawText(label, (MATRIX_W - w) / 2, (MATRIX_H - 5 * LABEL_SCALE) / 2 - 1,
                  PRESET_COLORS[_sys.net.myColorIndex()], LABEL_SCALE);
  drawSelectorDots(disp(), MATRIX_H - 1, VIRUSMENU_PAGES, _sel);
  disp().show();
}

// Which of my four goes to the match. Drawn in this device's own colour, which
// is also how the other players will see it on the board -- the letter is a
// local label and means nothing to anybody else's unit.
void VirusMenu::servicePick() {
  if (int8_t s = _sys.joy.stepX()) {
    _pick = (_pick + s + VIRUS_CHOICES) % VIRUS_CHOICES;
    _sys.audio.play(SFX_MOVE);
  }

  if (_sys.buttonB.wasPressed()) { _sys.audio.play(SFX_BACK); _state = SUB; return; }
  if (_sys.buttonA.wasPressed()) {
    _sys.audio.play(SFX_SELECT);
    // One rule, slot unknown until the lobby assigns one -- Virus::begin()
    // places it. Everyone else's decisions come off the wire.
    _game.setLocalRule(VIRUS_RULES[_pick], (uint8_t)_pick);
    _mp.begin();
    _state = MULTI;
    return;
  }

  disp().clear();
  const char letter = (char)('A' + _pick);
  const int  w = disp().textWidth("A", LABEL_SCALE);
  disp().drawChar(letter, (MATRIX_W - w) / 2, (MATRIX_H - 5 * LABEL_SCALE) / 2 - 1,
                  PRESET_COLORS[_sys.net.myColorIndex()], LABEL_SCALE);
  drawSelectorDots(disp(), MATRIX_H - 1, VIRUS_CHOICES, _pick);
  disp().show();
}
