#pragma once
#include "core/game.h"
#include "core/system.h"
#include "breakout_sim.h"

// ============================================================================
//  Breakout: the Game wrapper around BreakoutSim. Owns everything the sim is
//  deliberately blind to -- the menu, the serve/lost/cleared/over screens, the
//  joystick and buttons, rendering, audio and the high score.
//
//  Every rule lives in breakout_sim.cpp so it can be tested off-hardware; this
//  file holds no physics. The split is what lets the same suite run at 8x8 and
//  16x16 -- see docs/plans/breakout-plan.md.
// ============================================================================
class Breakout : public Game {
public:
  Breakout(System& sys) : _sys(sys) {}
  void       begin() override;
  GameStatus service() override;
  Icon       menuIcon() const override;

private:
  System&  _sys;
  Display& disp() { return _sys.display; }

  enum BState { B_MENU, B_SERVE, B_PLAYING, B_LIFE_LOST, B_CLEARED, B_GAMEOVER };
  BState   _state = B_MENU;
  bool     _exitReq = false;      // B_MENU: B pressed -> back to the main menu

  BreakoutSim _sim;

  uint32_t _lastTick = 0;         // fixed-step accumulator base
  uint32_t _accum = 0;
  uint32_t _stateStart = 0;       // when the current animation state began
  uint32_t _flashUntil = 0;       // white border after a speed-up / shrink

  uint32_t _highScore = 0;
  bool     _newHigh = false;

  int      _menuPage = 0;

  void startGame();
  void sound(uint8_t ev);
  void tickSim(uint32_t now);

  void updateMenu(uint32_t now);
  void updateServe(uint32_t now);
  void updatePlaying(uint32_t now);
  void updateLifeLost(uint32_t now);
  void updateCleared(uint32_t now);
  void updateGameOver(uint32_t now);

  void renderMenu();
  void renderBoard();             // bricks + paddle/lives + ball
  void renderLives();             // the paddle, with its leading N pixels standing in for lives
};
