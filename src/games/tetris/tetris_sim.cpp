#include "tetris_sim.h"

// ---- Tetris feel / tuning ---------------------------------------------------
// These moved here verbatim from tetris.cpp: they are rules, and the numbers a
// test asserts against have to be the numbers the game plays with. Presentation
// timings (clear animation, level flash, marquee speed) stayed behind.
#define FALL_START_MS    900     // gravity interval at level 0
#define FALL_MIN_MS      150
#define SOFTDROP_MS      60      // gravity interval while soft-drop held
#define LOCK_DELAY_MS    200     // grace before a grounded piece locks
#define LINES_PER_LEVEL  4
#define LEVEL_SPEEDUP_MS 100

// DAS -- delayed auto shift. A tap steps once; holding waits DAS_DELAY_MS and
// then repeats every ARR_MS. Rev 1 had a single uniform 55 ms gate because the
// slider gave an absolute target and the piece merely walked toward it; a
// direction that has to be HELD needs the two-rate shape, or a nudge to the
// next column and a slide to the wall become the same gesture.
#define DAS_DELAY_MS     170
#define ARR_MS            50

int TetrisSim::linesPerLevel() { return LINES_PER_LEVEL; }

static const int LINE_POINTS[5] = {0, 1, 3, 5, 8};  // compact, by rows cleared

static inline int clampI(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// ============================================================================
//  Lifecycle
// ============================================================================
void TetrisSim::newGame(uint8_t w, uint8_t h, uint32_t seed) {
  _w = w > TET_MAX_W ? TET_MAX_W : w;
  _h = h > TET_MAX_H ? TET_MAX_H : h;
  for (int r = 0; r < TET_MAX_H; r++)
    for (int c = 0; c < TET_MAX_W; c++) _board[r][c] = -1;
  for (int r = 0; r < TET_MAX_H; r++) _clearRows[r] = false;

  _rng = seed ? seed : 1;        // xorshift is a fixed point at zero
  _bagIndex = TET_NUM_TYPES;
  _score = 0;
  _lines = 0;
  _clearCount = 0;
  _gameOver = false;
  _softDropArmed = false;
  _grounded = false;
  _fallAcc = _lockAcc = _dasAcc = 0;
  _moveDir = 0;
  _dasPhase = 0;
  setLevel(0);
  _next = nextPiece();      // prime the lookahead, then draw from it
  spawnPiece();
}

// xorshift32. Enough for a piece bag, and reproducible from a seed, which is
// what lets a test assert on a bag sequence at all.
uint32_t TetrisSim::rnd() {
  _rng ^= _rng << 13;
  _rng ^= _rng >> 17;
  _rng ^= _rng << 5;
  return _rng;
}

void TetrisSim::setLevel(int lv) {
  _level = lv;
  uint32_t drop = (uint32_t)lv * LEVEL_SPEEDUP_MS;
  _fallInterval = drop >= FALL_START_MS - FALL_MIN_MS ? FALL_MIN_MS
                                                     : FALL_START_MS - drop;
}

// ============================================================================
//  Mechanics
// ============================================================================
bool TetrisSim::collides(int type, int rot, int px, int py) const {
  uint16_t s = SHAPES[type][rot];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (shapeCell(s, c, r)) {
        int bx = px + c, by = py + r;
        if (bx < 0 || bx >= _w) return true;
        if (by >= _h)           return true;
        if (by >= 0 && _board[by][bx] >= 0) return true;
      }
  return false;
}

void TetrisSim::lockPiece() {
  uint16_t s = SHAPES[_cur.type][_cur.rot];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (shapeCell(s, c, r)) {
        int by = _cur.y + r, bx = _cur.x + c;
        if (by >= 0 && by < _h && bx >= 0 && bx < _w) _board[by][bx] = (int8_t)_cur.type;
      }
}

int TetrisSim::detectFullRows() {
  int n = 0;
  for (int r = 0; r < _h; r++) {
    bool full = true;
    for (int c = 0; c < _w; c++) if (_board[r][c] < 0) { full = false; break; }
    _clearRows[r] = full;
    if (full) n++;
  }
  return n;
}

void TetrisSim::collapseRows() {
  int write = _h - 1;
  for (int read = _h - 1; read >= 0; read--) {
    if (_clearRows[read]) continue;
    if (write != read)
      for (int c = 0; c < _w; c++) _board[write][c] = _board[read][c];
    write--;
  }
  for (int r = write; r >= 0; r--)
    for (int c = 0; c < _w; c++) _board[r][c] = -1;
  for (int r = 0; r < _h; r++) _clearRows[r] = false;
}

void TetrisSim::refillBag() {
  for (int i = 0; i < TET_NUM_TYPES; i++) _bag[i] = i;
  for (int i = TET_NUM_TYPES - 1; i > 0; i--) {          // Fisher-Yates
    int j = (int)(rnd() % (uint32_t)(i + 1));
    int t = _bag[i]; _bag[i] = _bag[j]; _bag[j] = t;
  }
  _bagIndex = 0;
}

int TetrisSim::nextPiece() {
  if (_bagIndex >= TET_NUM_TYPES) refillBag();
  return _bag[_bagIndex++];
}

uint8_t TetrisSim::spawnPiece() {
  // Draw the piece already shown in the lookahead, then refill it. The dealt
  // sequence is identical to pulling straight from the bag -- the bag is just
  // read one piece ahead -- so the 7-bag fairness the tests pin is untouched.
  _cur.type = _next;
  _next     = nextPiece();
  _cur.rot  = 0;
  _cur.x    = (_w - 4) / 2;      // centered: 2 on an 8-wide board, 6 on a 16
  _cur.y    = -1;
  _softDropArmed = false;        // a held B does not carry into the next piece
  _grounded = false;
  _fallAcc = _lockAcc = 0;
  if (collides(_cur.type, _cur.rot, _cur.x, _cur.y)) {
    _gameOver = true;
    return TET_EV_GAMEOVER;
  }
  return 0;
}

int TetrisSim::ghostY() const {
  int gy = _cur.y;
  while (!collides(_cur.type, _cur.rot, _cur.x, gy + 1)) gy++;
  return gy;
}

// ============================================================================
//  Input
// ============================================================================
// A change of direction resets the DAS state machine, so reversing taps once
// immediately instead of inheriting the old direction's repeat phase.
void TetrisSim::setMove(int8_t dir) {
  dir = dir > 0 ? 1 : (dir < 0 ? -1 : 0);
  if (dir != _moveDir) {
    _moveDir  = dir;
    _dasPhase = 0;
    _dasAcc   = 0;
  }
}

void TetrisSim::tryStep(int8_t dir, uint8_t& ev) {
  if (!collides(_cur.type, _cur.rot, _cur.x + dir, _cur.y)) {
    _cur.x += dir;
    ev |= TET_EV_MOVE;
  }
}

// Soft drop is latched per piece rather than read as a plain held button: the
// latch is cleared on every spawn (see spawnPiece), so holding down through a
// lock does not slam the next piece down too. Re-press to arm it again.
void TetrisSim::softDrop(bool pressedEdge, bool held) {
  if (pressedEdge) _softDropArmed = true;
  if (!held)       _softDropArmed = false;
}

uint8_t TetrisSim::rotate(int8_t dir) {
  if (_gameOver || _clearCount) return 0;
  int nr = (_cur.rot + (dir < 0 ? 3 : 1)) & 3;
  static const int kicks[] = {0, -1, 1, -2, 2};
  for (int k : kicks)
    if (!collides(_cur.type, nr, _cur.x + k, _cur.y)) {
      _cur.rot = nr;
      _cur.x += k;
      return TET_EV_ROTATE;
    }
  return 0;
}

// Slam to the landing row and lock there, with no lock delay -- that is the
// whole point of a hard drop, and the reason it is worth a button of its own on
// a board this tall. One point per row travelled, the classic incentive to
// commit rather than to ride a piece down.
uint8_t TetrisSim::hardDrop() {
  if (_gameOver || _clearCount) return 0;

  const int gy = ghostY();
  const int rows = gy - _cur.y;
  if (rows > 0) _score += (uint32_t)rows;
  _cur.y = gy;

  uint8_t ev = TET_EV_HARDDROP | TET_EV_LOCK;
  lockPiece();
  _clearCount = detectFullRows();
  if (_clearCount) ev |= TET_EV_LINES;     // caller animates, then commits
  else             ev |= spawnPiece();
  return ev;
}

// ============================================================================
//  One update
// ============================================================================
uint8_t TetrisSim::update(uint32_t dtMs) {
  // Frozen while a clear is waiting to be committed, and after a game over.
  if (_gameOver || _clearCount) return 0;

  uint8_t ev = 0;

  // Horizontal movement, DAS. Phase 0 is the tap: it fires on the first update
  // after the direction appears, so a flick is always exactly one cell however
  // briefly it was held. Phase 1 waits out DAS_DELAY_MS, phase 2 repeats every
  // ARR_MS -- carrying the remainder rather than resetting the accumulator, so
  // the repeat rate does not drift with the caller's frame time.
  if (_moveDir == 0) {
    _dasPhase = 0;
    _dasAcc   = 0;
  } else if (_dasPhase == 0) {
    tryStep(_moveDir, ev);
    _dasPhase = 1;
    _dasAcc   = 0;
  } else {
    _dasAcc += dtMs;
    uint32_t gate = (_dasPhase == 1) ? DAS_DELAY_MS : ARR_MS;
    while (_dasAcc >= gate) {
      _dasAcc -= gate;
      tryStep(_moveDir, ev);
      _dasPhase = 2;
      gate = ARR_MS;
    }
  }

  // Gravity, with a lock delay so a piece can still be nudged after landing.
  if (collides(_cur.type, _cur.rot, _cur.x, _cur.y + 1)) {
    if (!_grounded) { _grounded = true; _lockAcc = 0; }
    else            { _lockAcc += dtMs; }
    if (_lockAcc >= LOCK_DELAY_MS) {
      lockPiece();
      ev |= TET_EV_LOCK;
      _clearCount = detectFullRows();
      if (_clearCount) ev |= TET_EV_LINES;    // caller animates, then commits
      else             ev |= spawnPiece();
    }
  } else {
    _grounded = false;
    uint32_t interval = _softDropArmed ? SOFTDROP_MS : _fallInterval;
    _fallAcc += dtMs;
    if (_fallAcc >= interval) {
      // Carry the remainder rather than zeroing it. Zeroing rounds every drop up
      // to the next frame boundary, so the real fall rate depends on how long a
      // frame happens to take -- and frame time is not a constant any more: the
      // panel went from 63 LEDs to 256, which tripled the time show() spends on
      // the wire. At the 150 ms floor with ~13 ms frames that was costing about
      // 4%. The DAS accumulator above already carries for the same reason.
      _fallAcc -= interval;
      // A stall must not bank rows. If something long-running (an NVS write, a
      // radio burst) swallowed several intervals, drop one row and start the
      // next interval clean: catching up by teleporting is worse than being
      // briefly late. Zero rather than a near-full carry, or the row after the
      // stall lands almost immediately -- which is the same teleport, deferred
      // by a frame.
      if (_fallAcc >= interval) _fallAcc = 0;
      _cur.y++;
    }
  }

  return ev;
}

// Collapse the latched rows, score them, and bring in the next piece. Scoring
// is per-clear and multiplied by the level, so a late four-row clear is worth
// many times an early single -- the reason to stack rather than to keep tidy.
uint8_t TetrisSim::commitClear() {
  if (!_clearCount) return 0;
  uint8_t ev = 0;

  collapseRows();
  _score += (uint32_t)LINE_POINTS[clampI(_clearCount, 0, 4)] * (uint32_t)(_level + 1);
  _lines += _clearCount;
  _clearCount = 0;

  int newLevel = _lines / LINES_PER_LEVEL;
  if (newLevel > _level) ev |= TET_EV_LEVEL;
  setLevel(newLevel);

  ev |= spawnPiece();
  return ev;
}

// ============================================================================
//  Reading the board
// ============================================================================
int8_t TetrisSim::cell(int c, int r) const {
  if (c < 0 || c >= _w || r < 0 || r >= _h) return -1;
  return _board[r][c];
}

bool TetrisSim::clearRow(int r) const {
  if (r < 0 || r >= _h) return false;
  return _clearRows[r];
}

void TetrisSim::placeCell(int c, int r, int8_t type) {
  if (c < 0 || c >= _w || r < 0 || r >= _h) return;
  _board[r][c] = type;
}

void TetrisSim::placePiece(int type, int rot, int x, int y) {
  _cur.type = clampI(type, 0, TET_NUM_TYPES - 1);
  _cur.rot  = rot & 3;
  _cur.x    = x;
  _cur.y    = y;
  _grounded = false;
  _fallAcc = _lockAcc = 0;
}
