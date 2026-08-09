#include "breakout_sim.h"

// ---- Breakout feel / tuning -------------------------------------------------
// Tuned on 8x8. Speed is expressed at h=8 and scaled by height in baseSpeed(),
// so a taller panel does not feel slower just because the ball has further to
// travel -- see the plan record for why that matters.
#define BASE_SPEED_Q8     30    // ~0.12 cells/tick, ~7.3 cells/s at 62 Hz
#define LEVEL_SPEEDUP_PCT 10    // added to base speed per level cleared
#define MAX_LEVEL_BONUS   9     // levels past this stop adding speed
#define MAX_SPEED_STEP     3
#define POINTS_BASE        1    // bottom brick row; +2 per row above it

// The horizontal fan: six unit directions at +/-65, +/-45 and +/-20 degrees
// off vertical, Q8. `sx` is signed (the direction's handedness), `cy` is the
// always-positive vertical magnitude -- _up decides its sign.
//
// There is no vertical entry and nothing flatter than 65 degrees ON PURPOSE.
// A perfectly vertical ball loops between one brick and the paddle forever and
// a near-horizontal one ping-pongs between the side walls out of reach; both
// are unrecoverable, and neither is representable here. Mirroring the table
// (index -> BRK_NUM_DIRS-1-index) is exactly a horizontal reflection, so a
// bounce off a wall is a table lookup rather than arithmetic, and no amount of
// bouncing can accumulate rounding drift.
struct BrkUnit { int16_t sx, cy; };
static const BrkUnit UNIT[BRK_NUM_DIRS] = {
  { -232, 108 },   // 0  steep left
  { -181, 181 },   // 1  45 left
  {  -88, 241 },   // 2  shallow left  (near vertical)
  {   88, 241 },   // 3  shallow right
  {  181, 181 },   // 4  45 right
  {  232, 108 },   // 5  steep right
};

static inline int32_t clampI32(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Board-derived geometry. These two functions are the entire 8x8 -> 16x16
// story for the playfield: 8 -> 2 rows / 3-wide paddle, 16 -> 4 rows / 6-wide.
static inline uint8_t brickRowsFor(uint8_t h) {
  uint8_t r = h / 4;
  if (r < 2) r = 2;
  if (r > BRK_MAX_ROWS) r = BRK_MAX_ROWS;
  return r;
}
static inline uint8_t paddleWidthFor(uint8_t w) {
  uint8_t p = (uint8_t)(w * 3 / 8);
  return p < 2 ? 2 : p;
}

// Paddle travel at full stick deflection, in Q8 cells per tick. Expressed as
// "ticks to cross the whole board" so it is a statement about feel rather than
// about pixels: ~0.8 s end to end at the 62 Hz step, on either panel.
#define PAD_CROSS_TICKS 50
static inline int32_t padSpeedFor(uint8_t w) {
  return (int32_t)w * BRK_FP / PAD_CROSS_TICKS;
}

// ============================================================================
//  Lifecycle
// ============================================================================
void BreakoutSim::newGame(uint8_t w, uint8_t h) {
  _w = w > BRK_MAX_W ? BRK_MAX_W : w;
  _h = h;
  _brickRows = brickRowsFor(_h);
  _score = 0;
  _lives = BRK_START_LIVES;
  _level = 0;
  _padPos = 0;
  resetScreen();
}

void BreakoutSim::nextLevel() {
  _level++;
  resetScreen();
}

// A fresh screen: bricks back, paddle back to full width, speed schedule and
// both "first time" flags rearmed. Score and lives deliberately survive.
void BreakoutSim::resetScreen() {
  for (int r = 0; r < BRK_MAX_ROWS; r++) _rows[r] = 0;
  uint16_t full = (uint16_t)((1u << _w) - 1u);
  for (int r = 0; r < _brickRows; r++) _rows[r] = full;
  _bricksLeft = (uint16_t)(_brickRows * _w);

  _padW = paddleWidthFor(_w);
  clampPaddle();
  _hits = 0;
  _speedStep = 0;
  _topWallHit = false;
  _topRowHit = false;
  applySpeed();
  park();
}

// Rest the ball on the paddle and aim the serve. The launch angle mirrors the
// paddle's position across the board -- far left serves steeply right, centre
// serves near-vertical -- so a serve always opens into free space instead of
// firing into the wall the player is already standing against.
void BreakoutSim::park() {
  _parked = true;
  _up = true;
  int32_t cx = (int32_t)_padCol * BRK_FP + (int32_t)_padW * BRK_FP / 2;
  int32_t z = (cx * BRK_NUM_DIRS) / ((int32_t)_w * BRK_FP);
  _dir = (uint8_t)(BRK_NUM_DIRS - 1 - clampI32(z, 0, BRK_NUM_DIRS - 1));
  _x = cx;
  _y = (int32_t)(_h - 1) * BRK_FP - 1;    // the row above the paddle
}

void BreakoutSim::launch() { _parked = false; }

// ============================================================================
//  Speed
// ============================================================================
int32_t BreakoutSim::baseSpeed() const {
  uint8_t lv = _level > MAX_LEVEL_BONUS ? MAX_LEVEL_BONUS : _level;
  int32_t s = (int32_t)BASE_SPEED_Q8 * _h / 8;
  return s * (100 + LEVEL_SPEEDUP_PCT * lv) / 100;
}

// Steps are x1.00, x1.25, x1.50, x1.75. The clamp is not cosmetic: step() does
// not sweep, so exceeding one cell per tick would let the ball pass through
// bricks. See BRK_MAX_SPEED.
void BreakoutSim::applySpeed() {
  _speed = clampI32(baseSpeed() * (4 + _speedStep) / 4, 1, BRK_MAX_SPEED);
}

void BreakoutSim::bumpSpeed(uint8_t& ev) {
  if (_speedStep >= MAX_SPEED_STEP) return;
  _speedStep++;
  applySpeed();
  ev |= BRK_EV_SPEEDUP;
}

// ============================================================================
//  Bricks
// ============================================================================
bool BreakoutSim::brick(int c, int r) const {
  if (c < 0 || c >= _w || r < 0 || r >= _brickRows) return false;
  return (_rows[r] >> c) & 1u;
}

// 0 = bottom brick row. Deeper rows score more and colour hotter, which is
// what makes tunnelling up a side both the clever play and the scoring play.
uint8_t BreakoutSim::brickTier(int r) const {
  return (uint8_t)(_brickRows - 1 - r);
}

// Destroy the brick at (c,r) if there is one, and run everything that hangs
// off a brick dying. Returns whether it hit, so the caller knows to reflect.
bool BreakoutSim::breakBrick(int c, int r, uint8_t& ev) {
  if (!brick(c, r)) return false;
  _rows[r] &= (uint16_t)~(1u << c);
  _bricksLeft--;
  _score += POINTS_BASE + 2u * brickTier(r);
  _hits++;
  ev |= BRK_EV_BRICK;

  // Atari's schedule was 4 and 12 hits, which on an 8x8 screen's 16 bricks is a
  // quarter and three quarters of the way through. Held as those FRACTIONS
  // rather than as absolute counts: 16x16 puts 64 bricks on screen, and 4 hits
  // into that is still the opening seconds rather than a milestone.
  const uint16_t total = (uint16_t)_brickRows * _w;
  if (_hits == total / 4 || _hits == total * 3 / 4) bumpSpeed(ev);
  if (r == 0 && !_topRowHit) { _topRowHit = true; bumpSpeed(ev); }
  if (_bricksLeft == 0) ev |= BRK_EV_CLEARED;
  return true;
}

// ============================================================================
//  Input
// ============================================================================
void BreakoutSim::clampPaddle() {
  const int32_t maxPos = (int32_t)(_w - _padW) * BRK_FP;
  _padPos = clampI32(_padPos, 0, maxPos);
  _padCol = (uint8_t)(_padPos >> 8);
}

// Deflection -> paddle velocity, integrated once per tick. The paddle still
// RENDERS in whole cells, and the deflection it imparts is still fine-grained
// because that comes from the BALL's sub-cell x -- what the sub-cell paddle
// position buys is that a small stick deflection can move it slowly instead of
// snapping a whole cell at a time.
void BreakoutSim::steerPaddle(int pct) {
  pct = (int)clampI32(pct, -100, 100);
  _padPos += (int32_t)pct * padSpeedFor(_w) / 100;
  clampPaddle();
}

void BreakoutSim::placePaddle(int col) {
  _padPos = (int32_t)col * BRK_FP;
  clampPaddle();
}

// Where on the paddle it landed -> which of the six directions it leaves at.
// This function is the game.
uint8_t BreakoutSim::englishZone() const {
  int32_t off  = _x - (int32_t)_padCol * BRK_FP;
  int32_t span = (int32_t)_padW * BRK_FP;
  int32_t z    = (off * BRK_NUM_DIRS) / span;
  return (uint8_t)clampI32(z, 0, BRK_NUM_DIRS - 1);
}

// ============================================================================
//  One tick
// ============================================================================
int BreakoutSim::ballCol() const {
  return (int)clampI32(_x >> 8, 0, _w - 1);
}
int BreakoutSim::ballRow() const {
  return (int)clampI32(_y >> 8, 0, _h - 1);
}

void BreakoutSim::placeBall(int32_t x, int32_t y, uint8_t dir, bool up) {
  _x = x; _y = y;
  _dir = (uint8_t)clampI32(dir, 0, BRK_NUM_DIRS - 1);
  _up = up;
  _parked = false;
}

uint8_t BreakoutSim::step() {
  uint8_t ev = 0;
  if (gameOver() || cleared()) return 0;

  // Parked: the ball rides the paddle so the player can aim the serve.
  if (_parked) {
    _x = (int32_t)_padCol * BRK_FP + (int32_t)_padW * BRK_FP / 2;
    _y = (int32_t)(_h - 1) * BRK_FP - 1;
    return 0;
  }

  const int32_t RIGHT = (int32_t)_w * BRK_FP;
  const int32_t PLANE = (int32_t)(_h - 1) * BRK_FP;   // top face of the paddle

  int32_t vx = (int32_t)UNIT[_dir].sx * _speed / BRK_FP;
  int32_t vy = (int32_t)UNIT[_dir].cy * _speed / BRK_FP;
  if (_up) vy = -vy;

  // ---- X ----------------------------------------------------------------
  // Reflecting first and testing the landed-in cell afterwards is safe because
  // the ball is never inside a brick: bricks die the moment it enters them, so
  // a wall reflection puts it back in a cell that is brick-free by definition.
  int32_t nx = _x + vx;
  if (nx < 0) {
    nx = -nx;
    _dir = (uint8_t)(BRK_NUM_DIRS - 1 - _dir);
    ev |= BRK_EV_WALL;
  } else if (nx >= RIGHT) {
    nx = 2 * RIGHT - nx - 1;
    _dir = (uint8_t)(BRK_NUM_DIRS - 1 - _dir);
    ev |= BRK_EV_WALL;
  }
  if (breakBrick((int)(nx >> 8), (int)(_y >> 8), ev)) {
    _dir = (uint8_t)(BRK_NUM_DIRS - 1 - _dir);   // hit a vertical face
  } else {
    _x = nx;
  }

  // ---- Y ----------------------------------------------------------------
  int32_t ny = _y + vy;
  if (ny < 0) {
    ny = -ny;
    _up = false;
    ev |= BRK_EV_WALL;
    // Classic: the paddle halves the first time the ball reaches the ceiling,
    // so breaking through to the top is a reward that immediately costs you.
    if (!_topWallHit) {
      _topWallHit = true;
      uint8_t nw = (uint8_t)(_padW / 2);
      if (nw < 2) nw = 2;
      if (nw != _padW) {
        _padW = nw;
        clampPaddle();          // a narrower paddle can still be off the right edge
        ev |= BRK_EV_SHRINK;
      }
    }
  }

  // The paddle catches by plane crossing, not cell overlap, so a miss lets the
  // ball travel visibly into the paddle row and out the bottom.
  if (!_up && _y < PLANE && ny >= PLANE) {
    int32_t left = (int32_t)_padCol * BRK_FP;
    if (_x >= left && _x < left + (int32_t)_padW * BRK_FP) {
      ny = 2 * PLANE - ny - 1;
      _up = true;
      _dir = englishZone();
      ev |= BRK_EV_PADDLE;
    }
  }

  if (breakBrick((int)(_x >> 8), (int)(ny >> 8), ev)) {
    _up = !_up;                                  // hit a horizontal face
  } else {
    _y = ny;
  }

  // ---- Lost? ------------------------------------------------------------
  if (_y >= (int32_t)_h * BRK_FP) {
    ev |= BRK_EV_LOST;
    if (_lives) _lives--;
    if (_lives) park();
    else { _parked = true; _y = (int32_t)(_h - 1) * BRK_FP - 1; }
  }

  return ev;
}
