#pragma once
#include <stdint.h>

// ============================================================================
//  The Breakout simulation: every rule, no hardware.
//
//  DELIBERATELY FREE OF <Arduino.h>, FastLED and Display. This is compiled by
//  the native test env (see platformio.ini) so the physics can be pinned at
//  both panel sizes without flashing a board -- which is the whole reason the
//  sim is a separate file from the Game. If this header ever pulls in the
//  Arduino core, `pio test -e native` stops building. Keep it stdint-only.
//
//  Resolution-agnostic: the board is sized by newGame(w, h) and nothing in
//  here reads MATRIX_W / MATRIX_H. Brick rows, paddle width and ball speed all
//  derive from those two numbers, so 8x8 -> 16x16 is a call-site change.
//
//  ---- Why the ball is not on the grid ------------------------------------
//  A ball that moves one whole cell per step can only travel at 45 degrees and
//  has exactly two possible directions, so the angle it leaves the paddle at
//  cannot depend on where it landed. That deflection *is* Breakout, so the
//  ball is a sub-pixel object instead: position and velocity in Q8 fixed point
//  (BRK_FP = one cell), rendered to the nearest cell.
//
//  Velocity is not a free vector. It is a table index:
//
//      _dir    0..5, the horizontal fan (steep left .. steep right)
//      _up     travelling up or down
//      _speed  magnitude, Q8 cells per tick
//
//  so every reflection is exact and drift-free -- side walls and brick side
//  faces mirror _dir, everything else flips _up. The table deliberately holds
//  no vertical entry and nothing flatter than 65 degrees off vertical, which
//  makes "the ball can never get stuck in a flat or vertical loop" a property
//  of the representation rather than a clamp bolted on afterwards.
// ============================================================================

#define BRK_MAX_W        16    // widest panel the brick bitmasks cover
#define BRK_MAX_ROWS      8    // brick rows -- max(2, h/4), so h up to 32
#define BRK_NUM_DIRS      6    // entries in the horizontal fan
#define BRK_FP          256    // one cell, in fixed point (Q8)
#define BRK_START_LIVES   3

// Ceiling on _speed. The axis-separated collision in step() does not sweep, so
// it is only free of tunnelling while a single tick moves the ball less than
// one cell on each axis. The largest unit component is 241/256, so 224 keeps
// the worst-case step at 211 -- comfortably inside a cell. test_breakout pins
// this; raise it and the ball starts passing through bricks.
#define BRK_MAX_SPEED   224

// What happened during one step(). The sim neither draws nor makes noise: it
// reports, and breakout.cpp maps these onto the sfx.h catalog. This is what
// keeps the sim linkable in the native test env.
enum BrkEvent : uint8_t {
  BRK_EV_WALL    = 1 << 0,   // bounced off a side or the ceiling
  BRK_EV_PADDLE  = 1 << 1,   // the paddle returned it
  BRK_EV_BRICK   = 1 << 2,   // at least one brick died
  BRK_EV_LOST    = 1 << 3,   // it got past the paddle; a life is gone
  BRK_EV_CLEARED = 1 << 4,   // that was the last brick on the screen
  BRK_EV_SPEEDUP = 1 << 5,   // the ball just got faster
  BRK_EV_SHRINK  = 1 << 6,   // the paddle just halved
};

class BreakoutSim {
public:
  // ---- lifecycle ----
  void newGame(uint8_t w, uint8_t h);   // fresh game: score, lives, level 0
  void nextLevel();                     // refill the screen, keep score+lives

  // ---- input ----
  // The stick sets paddle VELOCITY, not position. Absolute mapping is what rev
  // 1's pot did and it cannot survive a self-centering control: letting go
  // mid-rally would snap the paddle back to the middle of the board. Deflection
  // therefore means speed -- a light push nudges a fraction of a cell per tick,
  // full deflection crosses the board in about eight tenths of a second -- which
  // is also the one place on this device where an analog stick genuinely beats
  // a d-pad, since Breakout wants fine positioning and fast traverse constantly.
  void steerPaddle(int pct);            // -100..+100, applied once per tick
  void placePaddle(int col);            // test-only: put it somewhere exact
  void launch();                        // release a parked ball

  // ---- one tick ----
  uint8_t step();                       // -> BrkEvent bits

  // ---- state ----
  bool     parked()   const { return _parked; }
  bool     cleared()  const { return _bricksLeft == 0; }
  bool     gameOver() const { return _lives == 0; }
  uint8_t  lives()    const { return _lives; }
  uint8_t  level()    const { return _level; }
  uint32_t score()    const { return _score; }

  // ---- geometry, for rendering ----
  uint8_t w()          const { return _w; }
  uint8_t h()          const { return _h; }
  uint8_t brickRows()  const { return _brickRows; }
  bool    brick(int c, int r) const;
  uint8_t brickTier(int r) const;       // 0 = bottom brick row, up to rows-1
  int     paddleCol()   const { return _padCol; }
  uint8_t paddleWidth() const { return _padW; }
  int     ballCol()     const;
  int     ballRow()     const;

  // ---- physics, exposed for the tests ----
  // Rendering needs none of this; test_breakout asserts on all of it.
  int32_t ballX()  const { return _x; }
  int32_t ballY()  const { return _y; }
  uint8_t dir()    const { return _dir; }
  bool    movingUp() const { return _up; }
  int32_t speed()  const { return _speed; }
  // Test-only: drop the ball somewhere exact instead of playing to get there.
  void    placeBall(int32_t x, int32_t y, uint8_t dir, bool up);

private:
  uint8_t  _w = 0, _h = 0;
  uint8_t  _brickRows = 0;
  uint16_t _rows[BRK_MAX_ROWS] = { 0 };   // one bit per column, 1 = brick
  uint16_t _bricksLeft = 0;

  int32_t  _x = 0, _y = 0;     // ball, Q8, in cells
  uint8_t  _dir = 2;           // index into the fan
  bool     _up = true;
  int32_t  _speed = 0;         // Q8 cells per tick
  bool     _parked = true;     // resting on the paddle, awaiting launch

  // The paddle is sub-cell like the ball, for the same reason: velocity control
  // at low deflection has to be able to move less than a whole cell per tick or
  // slow is indistinguishable from stopped. _padCol is the rendered whole-cell
  // left edge, derived from _padPos and never written directly.
  int32_t  _padPos = 0;        // left edge, Q8, in cells
  uint8_t  _padCol = 0;        // left edge, whole cells
  uint8_t  _padW = 0;

  uint32_t _score = 0;
  uint8_t  _lives = 0;
  uint8_t  _level = 0;
  uint16_t _hits = 0;          // bricks broken this screen (speed schedule)
  uint8_t  _speedStep = 0;     // 0..3
  bool     _topWallHit = false;
  bool     _topRowHit = false;

  int32_t  baseSpeed() const;
  void     clampPaddle();      // hold _padPos on the board, refresh _padCol
  void     applySpeed();
  void     bumpSpeed(uint8_t& ev);
  void     resetScreen();
  void     park();
  uint8_t  englishZone() const;
  bool     breakBrick(int c, int r, uint8_t& ev);
};
