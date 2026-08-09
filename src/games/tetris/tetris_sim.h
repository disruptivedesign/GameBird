#pragma once
#include <stdint.h>
#include "shapes.h"

// ============================================================================
//  The Tetris simulation: the board, the active piece, gravity, lock delay,
//  line clears, scoring and levels. No hardware.
//
//  DELIBERATELY FREE OF <Arduino.h>, FastLED and Display, like BreakoutSim and
//  for the same reason: the native test env (see platformio.ini) compiles it so
//  the rules can be pinned off-hardware. Two consequences worth knowing:
//
//    - Time comes in as a delta. update(dtMs) advances internal accumulators
//      instead of comparing against millis(), so a test can hand it exactly one
//      gravity interval and assert the piece moved once.
//    - Randomness comes in as a seed. The 7-bag shuffles with an internal
//      xorshift rather than esp_random(), so a bag sequence is reproducible.
//      tetris.cpp passes esp_random() and gets the old behaviour.
//
//  Resolution-agnostic: the board is sized by newGame(w, h) and nothing here
//  reads MATRIX_W / MATRIX_H. The spawn column centers itself, so a 16x16 panel
//  needs no changes.
//
//  ---- The clear handshake -------------------------------------------------
//  A line clear is a rules event with an animation attached, and the animation
//  belongs to the caller. So update() latches the full rows and reports
//  TET_EV_LINES, the caller plays whatever it likes for as long as it likes
//  reading clearRow(), and then calls commitClear() to collapse the board and
//  spawn the next piece. Nothing advances in between.
// ============================================================================

#define TET_MAX_W  16
#define TET_MAX_H  16

enum TetEvent : uint8_t {
  TET_EV_MOVE     = 1 << 0,   // the piece stepped sideways
  TET_EV_ROTATE   = 1 << 1,
  TET_EV_LOCK     = 1 << 2,   // a piece became part of the board
  TET_EV_LINES    = 1 << 3,   // rows are latched; animate, then commitClear()
  TET_EV_LEVEL    = 1 << 4,   // level went up
  TET_EV_GAMEOVER = 1 << 5,   // a spawn had nowhere to go
  TET_EV_HARDDROP = 1 << 6,   // it was slammed down, not set down
};

class TetrisSim {
public:
  void newGame(uint8_t w, uint8_t h, uint32_t seed);

  // ---- input, once per tick ----------------------------------------------
  // Horizontal movement is DAS: setMove(-1/0/+1) reports the direction being
  // held, the first step fires immediately, and holding repeats at a fixed rate
  // after a delay. The sim owns that timing rather than the Game, so it can be
  // asserted off-hardware.
  //
  // Deliberately NOT modulated by how hard the stick is pushed, even though the
  // hardware could. Tetris placement is muscle memory -- "two taps and a hold
  // reaches the wall" has to stay true -- and a repeat rate that varies with
  // grip pressure makes every placement a fresh negotiation. Analog belongs on
  // Breakout's paddle, where the quantity being controlled is itself continuous.
  void setMove(int8_t dir);                      // -1 left, 0 none, +1 right
  void softDrop(bool pressedEdge, bool held);    // latched per piece (see .cpp)
  uint8_t rotate(int8_t dir);                    // +1 CW, -1 CCW -> TET_EV_ROTATE
  uint8_t hardDrop();                            // slam to the ghost and lock

  // ---- time ----
  uint8_t update(uint32_t dtMs);                 // -> TetEvent bits
  uint8_t commitClear();                         // after the caller's animation

  // ---- state ----
  bool     gameOver()     const { return _gameOver; }
  bool     clearPending() const { return _clearCount > 0; }
  int      clearCount()   const { return _clearCount; }
  bool     clearRow(int r) const;
  uint32_t score() const { return _score; }
  int      level() const { return _level; }
  int      lines() const { return _lines; }
  uint32_t fallInterval() const { return _fallInterval; }

  // For the info panel: what is coming, and how close the next level is.
  int        nextType() const { return _next; }
  static int linesPerLevel();
  int        linesIntoLevel() const { return _lines % linesPerLevel(); }

  // ---- geometry, for rendering ----
  uint8_t  w() const { return _w; }
  uint8_t  h() const { return _h; }
  int8_t   cell(int c, int r) const;        // -1 empty, else the piece type
  int      pieceType() const { return _cur.type; }
  int      pieceRot()  const { return _cur.rot; }
  int      pieceX()    const { return _cur.x; }
  int      pieceY()    const { return _cur.y; }
  uint16_t pieceShape() const { return SHAPES[_cur.type][_cur.rot]; }
  int      ghostY() const;                  // the row it would land on

  // ---- test-only: build a board position directly ----
  void placeCell(int c, int r, int8_t type);
  void placePiece(int type, int rot, int x, int y);

private:
  uint8_t _w = 0, _h = 0;
  int8_t  _board[TET_MAX_H][TET_MAX_W];
  struct { int type, rot, x, y; } _cur = { 0, 0, 0, 0 };

  uint32_t _rng = 1;
  int      _bag[TET_NUM_TYPES];
  int      _bagIndex = TET_NUM_TYPES;
  int      _next = 0;             // the lookahead the info panel draws

  uint32_t _fallInterval = 0;
  uint32_t _fallAcc = 0, _lockAcc = 0;
  bool     _grounded = false;
  bool     _softDropArmed = false;
  bool     _gameOver = false;

  // DAS: _moveDir is what is held, _dasPhase is 0 idle / 1 waiting out the
  // initial delay / 2 repeating, _dasAcc is time within the current phase.
  int8_t   _moveDir = 0;
  uint8_t  _dasPhase = 0;
  uint32_t _dasAcc = 0;

  uint32_t _score = 0;
  int      _level = 0, _lines = 0;
  bool     _clearRows[TET_MAX_H];
  int      _clearCount = 0;

  uint32_t rnd();
  bool     collides(int type, int rot, int px, int py) const;
  void     lockPiece();
  int      detectFullRows();
  void     collapseRows();
  void     refillBag();
  int      nextPiece();
  uint8_t  spawnPiece();
  void     tryStep(int8_t dir, uint8_t& ev);
  void     setLevel(int lv);
};
