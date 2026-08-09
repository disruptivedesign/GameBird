#include "tron.h"
#include "core/palette.h"
#include <string.h>

// ---- feel / tuning ----------------------------------------------------------
// Every number here was tuned at 8x8 and is now DERIVED from the arena, because
// a 16x16 board is four times the ground: the same 1.25 cells/s takes 12.8 s to
// cross it, and a 3-long trail in 256 cells is not an obstacle. Both the length
// scale and the speed scale go as the LINEAR dimension rather than the area --
// what a player experiences is how long it takes to get somewhere and how far
// back their own tail is, and both of those are lengths.
#define TRON_STEER_TH      30   // stick % past which a heading is requested
#define TRON_START_LEN_MIN  3   // never shorter than this, whatever the maths says

static inline uint16_t linearOf(uint8_t w, uint8_t h){
  uint16_t lin = (uint16_t)(w + h) / 2;
  return lin ? lin : 8;
}
// 3 at 8x8, 8 at 16x16.
static inline uint8_t startLenFor(uint8_t w, uint8_t h){
  uint32_t n = (uint32_t)w * h / 32;
  return (uint8_t)(n < TRON_START_LEN_MIN ? TRON_START_LEN_MIN : n);
}
// 150 ticks at 8x8 (~6 s at 25 Hz), 75 at 16x16 -- twice the ground to fill, so
// the trail has to grow about twice as fast to end a match in the same minute.
static inline uint16_t growTicksFor(uint8_t w, uint8_t h){
  return (uint16_t)(1200 / linearOf(w, h));
}
// 20 ticks/step at 8x8 (~1.25 cells/s), 10 at 16x16 (~2.5 cells/s). Boost halves
// it either way.
static inline uint8_t stepTicksFor(uint8_t w, uint8_t h, bool boost){
  uint16_t t = (uint16_t)((boost ? 80 : 160) / linearOf(w, h));
  return (uint8_t)(t < 2 ? 2 : t);
}

// Heading deltas: 0=right, 1=down, 2=left, 3=up. Reversing is dir^2, which is
// what makes the illegal-turn check below a single xor.
static const int8_t DX[4] = { 1, 0, -1, 0 };
static const int8_t DY[4] = { 0, 1, 0, -1 };

// Player render colors come from the host-resolved palette (see begin()); owner
// code c maps to player c-1, color _colors[c-1].

// ---- menu icon: two crossing light-trails ----
static const uint8_t TRON_ICON[8][8] = {
  {0,1,0,0,0,0,2,0},
  {0,1,0,0,0,0,2,0},
  {0,1,1,1,1,1,2,0},
  {0,0,0,0,0,1,2,0},
  {0,2,1,0,0,1,2,0},
  {0,2,0,0,0,1,0,0},
  {0,2,2,2,2,2,2,0},
  {0,0,0,0,0,0,0,0},
};
static const CRGB TRON_ICON_PAL[] = {
  CRGB::Black, CRGB(0, 200, 255), CRGB(255, 110, 0),
};

Icon Tron::menuIcon() const { return { TRON_ICON, TRON_ICON_PAL }; }

CRGB Tron::playerColor(uint8_t playerId) const {
  return playerId < NET_MAX_PLAYERS ? _colors[playerId] : CRGB(255, 255, 255);
}

// ============================================================================
//  Lifecycle
// ============================================================================
uint8_t Tron::maxLenNow() const {
  uint32_t n = _startLen + _tick / _growTicks;
  uint32_t cap = (uint32_t)_arenaW * _arenaH;
  if (cap > TRON_MAX_LEN - 1) cap = TRON_MAX_LEN - 1;
  if (n > cap) n = cap;
  return (uint8_t)n;
}

void Tron::spawn(uint8_t i, int x, int y, uint8_t dir){
  Player& p = _p[i];
  p.hx = x; p.hy = y; p.dir = dir;
  p.alive = true; p.boost = false;
  p.want = TRON_NO_TURN; p.stepCtr = 0; p.edgeGrace = false;
  p.head = 0; p.count = 1; p.trail[0] = cell(x, y);
  _owner[cell(x, y)] = i + 1;
}

void Tron::begin(uint8_t arenaW, uint8_t arenaH, uint8_t myId,
                 uint8_t numPlayers, uint32_t seed, const CRGB* colors){
  _arenaW = arenaW; _arenaH = arenaH;
  _myId = myId; _numPlayers = numPlayers; _seed = seed;

  // Cached once rather than recomputed per tick, and derived only from numbers
  // every role already has, so host and client agree without sending them.
  _startLen  = startLenFor(arenaW, arenaH);
  _growTicks = growTicksFor(arenaW, arenaH);
  _stepBase  = stepTicksFor(arenaW, arenaH, false);
  _stepBoost = stepTicksFor(arenaW, arenaH, true);

  _tick = 0; _phase = 0; _winner = NET_PID_NONE;
  for (int i = 0; i < NET_MAX_PLAYERS; i++)
    _colors[i] = colors ? colors[i] : PRESET_COLORS[i % NUM_PRESET_COLORS];
  memset(_owner, 0, sizeof(_owner));
  for (int i = 0; i < NET_MAX_PLAYERS; i++) _p[i].alive = false;

  int W = arenaW, H = arenaH;
  const int sx[5] = { 2,   W - 3, W / 2, W / 2, 2 };
  const int sy[5] = { H/2, H / 2, 2,     H - 3, 2 };
  const uint8_t sd[5] = { 0, 2, 1, 3, 1 };
  for (uint8_t i = 0; i < numPlayers && i < NET_MAX_PLAYERS; i++)
    spawn(i, sx[i], sy[i], sd[i]);
}

// ============================================================================
//  Trail ring buffer
// ============================================================================
void Tron::trailAdd(uint8_t i, int c){
  Player& p = _p[i];
  p.head = (p.head + 1) % TRON_MAX_LEN;
  p.trail[p.head] = (uint16_t)c;
  if (p.count < TRON_MAX_LEN) p.count++;
  _owner[c] = i + 1;
}

void Tron::trailTrim(uint8_t i, uint8_t maxLen){
  Player& p = _p[i];
  while (p.count > maxLen){
    int oldest = (p.head - p.count + 1 + TRON_MAX_LEN) % TRON_MAX_LEN;
    uint16_t c = p.trail[oldest];
    if (_owner[c] == i + 1) _owner[c] = 0;   // free the vacated tail cell
    p.count--;
  }
}

// ============================================================================
//  Input (client-produced, host-consumed)
// ============================================================================
// Stick -> desired heading. Diagonals resolve to the DOMINANT axis, and an
// exact tie requests nothing at all.
//
// Stateless on purpose. Remembering the last unambiguous direction would smooth
// over sloppy diagonals, but it would put hidden input history inside a NetGame
// -- and it is not needed: "no request" already means "keep going", so a tie
// coasts rather than dropping the player somewhere they did not aim.
size_t Tron::serializeInput(uint8_t* buf, size_t cap, const LocalInput& in){
  if (cap < 2) return 0;

  const int mx = in.x < 0 ? -in.x : in.x;
  const int my = in.y < 0 ? -in.y : in.y;

  uint8_t want = TRON_NO_TURN;
  if (mx >= TRON_STEER_TH || my >= TRON_STEER_TH){
    if      (mx > my) want = in.x > 0 ? TRON_DIR_R : TRON_DIR_L;
    else if (my > mx) want = in.y > 0 ? TRON_DIR_U : TRON_DIR_D;  // +y is up
  }

  buf[0] = want;
  buf[1] = in.a ? 1 : 0;
  return 2;
}

void Tron::applyInput(uint8_t pid, const uint8_t* buf, size_t len){
  if (pid >= _numPlayers || len < 2) return;
  Player& p = _p[pid];
  p.want  = buf[0];           // held: re-applied on every step while off-center
  p.boost = buf[1] != 0;
}

// ============================================================================
//  AI: greedy open-space avoidance. Produces the same {steer, boost} bytes a
//  human would, so it drives through the identical applyInput() path.
// ============================================================================

// Count empty cells reachable from (sx,sy) via 4-way flood fill.
int Tron::floodArea(int sx, int sy) const {
  if (sx < 0 || sx >= _arenaW || sy < 0 || sy >= _arenaH) return 0;
  bool     seen[TRON_MAX_CELLS] = {false};
  uint16_t q[TRON_MAX_CELLS];
  int qh = 0, qt = 0, count = 0;
  int start = sy * _arenaW + sx;
  seen[start] = true; q[qt++] = (uint16_t)start;
  while (qh < qt){
    int c = q[qh++]; count++;
    int cx = c % _arenaW, cy = c / _arenaW;
    for (int k = 0; k < 4; k++){
      int nx = cx + DX[k], ny = cy + DY[k];
      if (nx < 0 || nx >= _arenaW || ny < 0 || ny >= _arenaH) continue;
      int nc = ny * _arenaW + nx;
      if (_owner[nc] == 0 && !seen[nc]){ seen[nc] = true; q[qt++] = (uint16_t)nc; }
    }
  }
  return count;
}

size_t Tron::aiInput(uint8_t pid, uint8_t* buf, size_t cap){
  if (cap < 2) return 0;
  buf[1] = 0;                                  // no boost in v1
  if (pid >= _numPlayers || !_p[pid].alive){ buf[0] = TRON_NO_TURN; return 2; }

  const Player& p = _p[pid];
  const uint8_t cand[3] = { p.dir, (uint8_t)((p.dir + 3) & 3), (uint8_t)((p.dir + 1) & 3) };
  int bestK = 0, bestScore = -1;               // straight (k=0) wins ties -> no twitching
  for (int k = 0; k < 3; k++){
    int nx = p.hx + DX[cand[k]], ny = p.hy + DY[cand[k]];
    int score;
    if (nx < 0 || nx >= _arenaW || ny < 0 || ny >= _arenaH) score = -1;   // wall = death
    else if (_owner[ny * _arenaW + nx] != 0) score = -1;                  // trail = death
    else score = floodArea(nx, ny);            // prefer the roomiest escape
    if (score > bestScore){ bestScore = score; bestK = k; }
  }
  // The candidates ARE headings, so the AI can say what it wants directly --
  // rev 1 converted this back into a relative turn purely because that was all
  // the wire could carry.
  buf[0] = cand[bestK];
  return 2;
}

// ============================================================================
//  Authoritative simulation
// ============================================================================
void Tron::hostTick(){
  if (_phase != 0) return;
  _tick++;
  uint8_t maxLen = maxLenNow();

  bool    step[NET_MAX_PLAYERS] = {false};
  bool    die [NET_MAX_PLAYERS] = {false};
  int16_t tx[NET_MAX_PLAYERS], ty[NET_MAX_PLAYERS];

  // 1. who steps this tick, and where to
  for (uint8_t i = 0; i < _numPlayers; i++){
    Player& p = _p[i];
    if (!p.alive) continue;
    uint8_t every = p.boost ? _stepBoost : _stepBase;
    if (++p.stepCtr < every) continue;
    p.stepCtr = 0;
    // Adopt the requested heading, unless it is a reversal. Rejected HERE and
    // not in the UI: a reversal walks straight into your own neck, and the
    // client that sent it is not trusted to have checked.
    if (p.want < 4 && p.want != ((p.dir + 2) & 3)) p.dir = p.want;
    step[i] = true;
    tx[i] = p.hx + DX[p.dir];
    ty[i] = p.hy + DY[p.dir];
  }

  // 2. wall / existing-trail collisions
  //
  // The wall gets one grace step: the first time a player's next cell would
  // be off the arena, they are held in place -- not killed -- for the rest of
  // this step interval, which is exactly the window a turn away needs, since
  // heading changes are adopted at the top of section 1 on every step. Only a
  // SECOND consecutive attempt at the wall, meaning they never turned, costs
  // the match. A trail collision has no such grace -- it means a cell is
  // already spoken for, not a boundary a turn could still avoid.
  for (uint8_t i = 0; i < _numPlayers; i++){
    if (!step[i]) continue;
    bool offBoard = tx[i] < 0 || tx[i] >= _arenaW || ty[i] < 0 || ty[i] >= _arenaH;
    if (offBoard){
      if (!_p[i].edgeGrace){ _p[i].edgeGrace = true; step[i] = false; continue; }
      die[i] = true;
    } else {
      _p[i].edgeGrace = false;
      if (_owner[cell(tx[i], ty[i])] != 0) die[i] = true;
    }
  }

  // 3. head-on: two heads entering the same cell this tick
  for (uint8_t i = 0; i < _numPlayers; i++)
    for (uint8_t j = i + 1; j < _numPlayers; j++)
      if (step[i] && step[j] && tx[i] == tx[j] && ty[i] == ty[j]){ die[i] = die[j] = true; }

  // 4. commit
  for (uint8_t i = 0; i < _numPlayers; i++){
    if (!step[i]) continue;
    if (die[i]){ _p[i].alive = false; continue; }
    _p[i].hx = tx[i]; _p[i].hy = ty[i];
    trailAdd(i, cell(tx[i], ty[i]));
    trailTrim(i, maxLen);
  }

  // 5. win check (needs at least 2 starters)
  if (_numPlayers >= 2){
    int aliveCount = 0, last = NET_PID_NONE;
    for (uint8_t i = 0; i < _numPlayers; i++) if (_p[i].alive){ aliveCount++; last = i; }
    if (aliveCount <= 1){ _phase = 1; _winner = (aliveCount == 1) ? last : NET_PID_NONE; }
  }
}

// ============================================================================
//  State (host-produced, client-consumed). Nibble-packed owner map keeps the
//  16x16 snapshot well under the ESP-NOW 250-byte cap.
// ============================================================================
size_t Tron::serializeState(uint8_t* buf, size_t cap){
  int cells = _arenaW * _arenaH;
  size_t need = 3 + (size_t)_numPlayers * 3 + (cells + 1) / 2;
  if (cap < need) return 0;

  size_t o = 0;
  buf[o++] = _phase;
  buf[o++] = _winner;
  buf[o++] = _numPlayers;
  for (uint8_t i = 0; i < _numPlayers; i++){
    buf[o++] = (uint8_t)_p[i].hx;
    buf[o++] = (uint8_t)_p[i].hy;
    buf[o++] = (uint8_t)((_p[i].dir & 3) | (_p[i].alive ? 0x80 : 0));
  }
  for (int c = 0; c < cells; c += 2){
    uint8_t lo = _owner[c] & 0x0F;
    uint8_t hi = (c + 1 < cells) ? (_owner[c + 1] & 0x0F) : 0;
    buf[o++] = lo | (hi << 4);
  }
  return o;
}

void Tron::applyState(const uint8_t* buf, size_t len){
  if (len < 3) return;
  size_t o = 0;
  _phase      = buf[o++];
  _winner     = buf[o++];
  _numPlayers = buf[o++];
  if (_numPlayers > NET_MAX_PLAYERS) _numPlayers = NET_MAX_PLAYERS;
  for (uint8_t i = 0; i < _numPlayers; i++){
    if (o + 3 > len) return;
    _p[i].hx = buf[o++];
    _p[i].hy = buf[o++];
    uint8_t d = buf[o++];
    _p[i].dir = d & 3;
    _p[i].alive = (d & 0x80) != 0;
  }
  int cells = _arenaW * _arenaH;
  memset(_owner, 0, sizeof(_owner));
  for (int c = 0; c < cells; c += 2){
    if (o >= len) break;
    uint8_t byte = buf[o++];
    _owner[c] = byte & 0x0F;
    if (c + 1 < cells) _owner[c + 1] = (byte >> 4) & 0x0F;
  }
}

// ============================================================================
//  Render -- identical on every unit since all apply the same owner map.
// ============================================================================
void Tron::render(Display& disp) const {
  disp.clear();
  for (int y = 0; y < _arenaH; y++)
    for (int x = 0; x < _arenaW; x++){
      uint8_t c = _owner[y * _arenaW + x];
      if (c) disp.setPixel(x, y, _colors[c - 1]);
    }
  // heads drawn bright white for legibility
  for (uint8_t i = 0; i < _numPlayers; i++)
    if (_p[i].alive) disp.setPixel(_p[i].hx, _p[i].hy, CRGB(255, 255, 255));

  if (_phase != 0 && _winner != NET_PID_NONE)
    disp.border(_colors[_winner]);            // winner's color frames the result
}
