#pragma once
#include "core/game.h"
#include "core/system.h"
#include "tetris_sim.h"

// ============================================================================
//  Tetris: the Game wrapper around TetrisSim. Owns its menu, the clear and
//  game-over screens, input, rendering and the high score. Every rule -- the
//  board, gravity, lock delay, the 7-bag, scoring, levels -- lives in
//  tetris_sim.cpp so it can be tested off-hardware.
// ============================================================================
class Tetris : public Game {
public:
  Tetris(System& sys) : _sys(sys) {}
  void begin() override;
  GameStatus service() override;
  Icon menuIcon() const override;

private:
  System&  _sys;
  Display& disp() { return _sys.display; }

  enum TState { T_MENU, T_PLAYING, T_CLEARING, T_GAMEOVER };
  TState   _state = T_MENU;
  bool     _exitReq = false;      // T_MENU: B pressed -> back to main menu

  TetrisSim _sim;
  uint32_t  _lastUpdate = 0;      // for the dt handed to TetrisSim::update

  // Presentation timing (the sim holds none of this)
  uint32_t _levelFlashUntil = 0;
  uint32_t _clearStart = 0, _gameOverStart = 0;

  // Hard-drop arming: the stick has to settle near centre before an up-flick
  // counts, so releasing a soft drop cannot slam the piece. See servicePlaying.
  bool     _dropArmed = true;
  uint32_t _neutralAt = 0;

  // Menu / mode
  int      _menuPage = 0;
  bool     _ghostEnabled = false;

  uint32_t _highScore = 0;
  bool     _newHigh = false;

  void startGame();
  void enterGameOver(uint32_t now);   // latch + persist the high score
  void sound(uint8_t ev);             // TetEvent bits -> the buzzer

  void updateMenu(uint32_t now);
  void updateClearing(uint32_t now);
  void updateGameOver(uint32_t now);
  void servicePlaying(uint32_t now);

  void render();
  void renderMenu();
  void renderClearing(uint32_t t);
  void renderPiece(uint16_t shape, int px, int py, const CRGB& c);
  void renderHud();                       // divider + next / level / lines panel
  void renderNext(int x0, int y0);        // the lookahead, centered in a 4x4 box
};
