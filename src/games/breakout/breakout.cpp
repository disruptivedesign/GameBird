#include "breakout.h"
#include "bricks.h"           // BRICK_COLORS, ICON_PAL, IMG_*
#include "audio/sfx.h"
#include "audio/notes.h"
#include "ui/scoreboard.h"    // drawScoreScroll / drawScoreScrollLoop

// ---- Breakout presentation / timing (the rules live in breakout_sim.cpp) ----
#define TICK_MS          16     // ~62 Hz fixed physics step
#define MAX_CATCHUP      4      // ticks per service(); stops a stall spiralling
#define LOST_FLASH_MS    400    // red flash after losing a ball
#define LOST_LIVES_MS    600    // then: how many are left
#define CLEARED_MS       900    // screen-cleared celebration
#define OVER_FLASH_MS    700
#define FLASH_MS         180    // border flash on speed-up / paddle shrink
#define NUM_MENU_PAGES   2      // 0 = play, 1 = high score

#define NS_BREAKOUT  "breakout"
#define KEY_HIGH     "hi"

// Breakout's own two voices. The catalog in sfx.h covers the moments Breakout
// shares with every other game (SFX_HIT for a brick, SFX_DEATH, SFX_WIN,
// SFX_LOSE), but a bounce has no equivalent anywhere else on the device -- and
// it is by far the most-played sound here, so both are as short as the piezo
// can articulate. The paddle sits a fifth above the wall so a return is
// audibly the good outcome.
static const Step S_WALL[]   = { {NOTE_G5, 12} };
static const Step S_PADDLE[] = { {NOTE_D6, 18} };
static const Sfx  SFX_BRK_WALL   = SFX_OF(S_WALL,   PRIO_MINOR);
static const Sfx  SFX_BRK_PADDLE = SFX_OF(S_PADDLE, PRIO_MINOR);

// ============================================================================
//  Lifecycle
// ============================================================================
void Breakout::begin() {
  _highScore = _sys.storage.getU32(NS_BREAKOUT, KEY_HIGH, 0);
  _state = B_MENU;
  _menuPage = 0;
  _exitReq = false;
}

Icon Breakout::menuIcon() const { return { IMG_BREAKOUT, ICON_PAL }; }

GameStatus Breakout::service() {
  uint32_t now = millis();

  // Exit to the submenu from anywhere mid-match, same convention as every
  // other game. Serve and Playing are the only states with live player
  // control; Life Lost and Cleared are brief automatic beats, and Game Over
  // already reaches the same menu on its own skip gesture.
  if ((_state == B_SERVE || _state == B_PLAYING) && _sys.buttonB.wasPressed()) {
    _sys.audio.play(SFX_BACK);
    _state = B_MENU;
    _menuPage = 0;
    return GAME_CONTINUE;
  }

  switch (_state) {
    case B_MENU:      updateMenu(now);     return _exitReq ? GAME_EXIT : GAME_CONTINUE;
    case B_SERVE:     updateServe(now);    break;
    case B_PLAYING:   updatePlaying(now);  break;
    case B_LIFE_LOST: updateLifeLost(now); break;
    case B_CLEARED:   updateCleared(now);  break;
    case B_GAMEOVER:  updateGameOver(now); break;
  }
  return GAME_CONTINUE;
}

void Breakout::startGame() {
  _sim.newGame(MATRIX_W, MATRIX_H);
  _newHigh = false;
  _flashUntil = 0;
  _lastTick = millis();
  _accum = 0;
  _state = B_SERVE;
}

// ============================================================================
//  Driving the sim
// ============================================================================
// One sound per tick, loudest moment wins. The sim reports what happened and
// nothing more, so this is the only place Breakout's events meet the buzzer.
void Breakout::sound(uint8_t ev) {
  if      (ev & BRK_EV_LOST)    _sys.audio.play(SFX_DEATH);
  else if (ev & BRK_EV_CLEARED) _sys.audio.play(SFX_WIN);
  else if (ev & BRK_EV_BRICK)   _sys.audio.play(SFX_HIT);
  else if (ev & BRK_EV_PADDLE)  _sys.audio.play(SFX_BRK_PADDLE);
  else if (ev & BRK_EV_WALL)    _sys.audio.play(SFX_BRK_WALL);
}

// Fixed-step accumulator: physics advances in whole TICK_MS steps regardless of
// how long the main loop actually took, so the ball travels at the same speed
// on a busy frame as a quiet one. MAX_CATCHUP caps the debt a long stall can
// build up -- without it, one slow frame fires a burst of ticks and the ball
// teleports through a brick.
void Breakout::tickSim(uint32_t now) {
  _accum += now - _lastTick;
  _lastTick = now;
  if (_accum > TICK_MS * MAX_CATCHUP) _accum = TICK_MS * MAX_CATCHUP;

  while (_accum >= TICK_MS) {
    _accum -= TICK_MS;
    _sim.steerPaddle(_sys.joy.x());
    uint8_t ev = _sim.step();
    if (ev) {
      sound(ev);
      if (ev & (BRK_EV_SPEEDUP | BRK_EV_SHRINK)) _flashUntil = now + FLASH_MS;
      if (ev & BRK_EV_LOST) {
        _stateStart = now;
        _state = _sim.gameOver() ? B_GAMEOVER : B_LIFE_LOST;
        if (_state == B_GAMEOVER) {
          // Here rather than in updateGameOver: that state is entered on a
          // later tick, so there is no frame where its elapsed time is zero.
          // PRIO_MATCH outranks the SFX_DEATH that sound() just started.
          _sys.audio.play(SFX_LOSE);
          _newHigh = _sim.score() > _highScore;
          if (_newHigh) {
            _highScore = _sim.score();
            _sys.storage.putU32(NS_BREAKOUT, KEY_HIGH, _highScore);
          }
        }
        return;
      }
      if (ev & BRK_EV_CLEARED) { _stateStart = now; _state = B_CLEARED; return; }
    }
  }
}

// ============================================================================
//  States
// ============================================================================
void Breakout::updateMenu(uint32_t now) {
  (void)now;
  if (int8_t s = _sys.joy.stepX()) {
    _menuPage = (_menuPage + s + NUM_MENU_PAGES) % NUM_MENU_PAGES;
    _sys.audio.play(SFX_MOVE);
  }

  if (_sys.buttonB.wasPressed()) { _sys.audio.play(SFX_BACK); _exitReq = true; return; }
  if (_sys.buttonA.wasPressed() && _menuPage == 0) {
    _sys.audio.play(SFX_SELECT);
    startGame();
    return;
  }
  renderMenu();
}

// The ball rides the paddle until A. Serving is already a decision -- the
// launch angle mirrors where the paddle is standing -- so this is aiming time,
// not dead time.
//
// A flick UP serves as well. The gesture is right there and means the same
// thing, and it costs nothing here: the stick is not doing anything else during
// a serve, and stepY is an edge, so leaning on it cannot re-fire.
void Breakout::updateServe(uint32_t now) {
  _lastTick = now;
  _accum = 0;
  _sim.steerPaddle(_sys.joy.x());
  _sim.step();                          // keeps the parked ball on the paddle

  if (_sys.buttonA.wasPressed() || _sys.joy.stepY() < 0) {
    _sim.launch();
    _state = B_PLAYING;
  }
  renderBoard();
}

void Breakout::updatePlaying(uint32_t now) {
  tickSim(now);
  if (_state == B_PLAYING) renderBoard();
}

void Breakout::updateLifeLost(uint32_t now) {
  uint32_t t = now - _stateStart;

  if (t < LOST_FLASH_MS) {
    if ((t / 120) & 1) disp().clear();
    else               disp().fill(CRGB(120, 0, 0));
    disp().show();
    return;
  }
  if (t < LOST_FLASH_MS + LOST_LIVES_MS) {
    renderBoard();
    return;
  }
  _state = B_SERVE;
}

void Breakout::updateCleared(uint32_t now) {
  uint32_t t = now - _stateStart;
  if (t < CLEARED_MS) {
    // Sweep a bright bar up the panel: the screen you just took apart, put
    // back. Reads as progress rather than as the red "you lost" flash.
    int row = (int)(MATRIX_H - 1 - (t * MATRIX_H) / CLEARED_MS);
    disp().clear();
    for (int c = 0; c < MATRIX_W; c++) {
      if (row >= 0 && row < MATRIX_H) disp().setPixel(c, row, CRGB(230, 230, 230));
    }
    disp().show();
    return;
  }
  _sim.nextLevel();
  _lastTick = now;
  _accum = 0;
  _state = B_SERVE;
}

void Breakout::updateGameOver(uint32_t now) {
  uint32_t t = now - _stateStart;
  bool skip = _sys.buttonA.wasPressed() || _sys.buttonB.wasPressed();

  if (t < OVER_FLASH_MS) {
    if ((t / 150) & 1) disp().clear();
    else               disp().fill(CRGB::Red);
    disp().show();
    return;
  }

  disp().clear();
  bool done = drawScoreScroll(disp(), _sim.score(), t - OVER_FLASH_MS,
                              _newHigh ? CRGB(255, 180, 0) : CRGB(0, 150, 255));
  disp().show();

  // Back to Breakout's own menu rather than straight into another game, so the
  // top-level menu is reachable without the quit gesture.
  if (done || skip) {
    _state = B_MENU;
    _menuPage = 0;
  }
}

// ============================================================================
//  Rendering
// ============================================================================
void Breakout::renderMenu() {
  disp().clear();
  if (_menuPage == 0) {
    disp().drawBitmap(IMG_BREAKOUT, ICON_PAL);
  } else if (_highScore == 0) {
    disp().drawBitmap(IMG_SCORE, ICON_PAL);
  } else {
    drawScoreScrollLoop(disp(), _highScore, millis(), CRGB(255, 180, 0));
  }
  disp().show();
}

void Breakout::renderBoard() {
  disp().clear();

  for (int r = 0; r < _sim.brickRows(); r++)
    for (int c = 0; c < _sim.w(); c++)
      if (_sim.brick(c, r)) disp().setPixel(c, r, brickColor(_sim.brickTier(r)));

  renderLives();

  disp().setPixel(_sim.ballCol(), _sim.ballRow(), BALL_COLOR);

  if (millis() < _flashUntil) disp().border(CRGB::White);
  disp().show();
}

// Lives AND the paddle, together: the leading _sim.lives() pixels of the
// paddle stand in for the life gauge, the rest stay PADDLE_COLOR. Used to be
// its own row under the bricks, permanently sitting in the ball's travel
// corridor -- every trip between the bricks and the paddle crossed it. Riding
// on the paddle instead costs no extra row and only ever overlaps the ball at
// the exact moment the ball is bouncing off the paddle anyway.
//
// Lives can never exceed paddle width in practice (BRK_START_LIVES is 3;
// paddleWidthFor is at least 2, and reaches 3 at the smallest board this ever
// runs on), but the loop still clamps to i < paddleWidth() rather than assume
// it, since a life indicator silently overflowing onto the ball's row would
// be a much stranger bug to track down than the gauge running out of pixels.
void Breakout::renderLives() {
  int py = _sim.h() - 1;
  uint8_t lives = _sim.lives();
  for (int i = 0; i < _sim.paddleWidth(); i++)
    disp().setPixel(_sim.paddleCol() + i, py, i < lives ? LIFE_COLOR : PADDLE_COLOR);
}
