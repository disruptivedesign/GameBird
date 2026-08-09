#include "virus.h"
#include "virus_voice.h"      // VIRUS_VOICES -- what speak() plays
#include "core/palette.h"
#include <string.h>

// Per-tick standings to Serial (USB, 115200) so a virus author can see why a
// match went the way it did. Set to 0 (or -DVIRUS_TELEMETRY=0) to silence.
#ifndef VIRUS_TELEMETRY
#define VIRUS_TELEMETRY 1
#endif

// Openings used to be rolled from the seed. They are a lookup now -- see
// openingSpot() below -- so nothing here needs a PRNG and the whole sim is
// reproducible from (opening, roster) alone.

// ---- ruleset tunables (spec §12) -------------------------------------------
// Energy is PER CELL. A virus earns an income every tick, that income is split
// evenly across the cells it owns, and each cell banks its own share until it
// can afford something. A cell spends from its own purse and from nothing else.
// Action costs live on Action::cost() in virus_api.h (Grow 2, Move/Fortify/
// Attack 1, Spore 6).
//
// WHY THE SHARE ACCUMULATES. Handing each cell income/cells outright does not
// work: at 224 tiles the income is 10 and a 40-cell virus would get 10/40 = 0
// per cell, forever. The share is therefore banked in 1/256ths, so a cell on a
// crowded board earns a trickle and acts every Nth tick rather than never.
//
// INCOME SCALES WITH BOARD AREA, NOT WITH THE VIRUS. That is what keeps a big
// virus from out-earning a small one: splitting a fixed income across more cells
// makes each cell poorer, so growth pays for itself in area rather than income.
// Scaling with board area (and not with cost) keeps the per-cell economy the
// same shape at 64 tiles and at 224.
//
// VIRUS_ENERGY_BASE was 3 while a shared pool plus Priority let a virus route
// every point of income to the cells that mattered. Per-cell shares also pay
// interior cells that have nothing to spend on -- they fill to VIRUS_CELL_BANK
// and waste the rest -- so the useful spend rate is lower for the same income.
//
// Swept 2..6 against the four starters on 16x14 before settling on 4. The board
// reaches ~90% occupancy at every value in that range, so the feared "it will
// no longer fill in time" does not happen and the compensation does not need to
// be large. What income actually buys is PACE: 2 takes about a third longer to
// reach the same board than 6 does. Matches end on the stalemate detector well
// before the clock either way, so raise this to make a match brisker and lower
// it to give slow strategies more room, rather than to control the ending.
#ifndef VIRUS_ENERGY_BASE          // -DVIRUS_ENERGY_BASE=n to sweep it without editing
#define VIRUS_ENERGY_BASE     4    // income per tick on a 64-cell board
#endif
#define VIRUS_CELL_BANK       8    // cap on one cell's purse: a Spore (6) plus a Grow (2)
#define VIRUS_START_STRENGTH  1    // seed cell strength at match start
#define VIRUS_GROW_STRENGTH   1    // strength of a cell created by Grow
#define VIRUS_STRENGTH_MAX    4

// Energy is banked in 1/256ths so a fractional per-tick share accumulates
// exactly, with no floating point anywhere in the simulation.
static constexpr uint16_t ENERGY_ONE = 256;
static constexpr uint32_t BANK_MAX_Q8 = (uint32_t)VIRUS_CELL_BANK * ENERGY_ONE;

// What a cell has to spend once this tick's share is credited and the cap
// applied. Both the referee and the host's report to a client go through here,
// so the number a rule sees is the number that will actually be charged.
static inline uint32_t creditedBank(uint16_t bank, uint32_t share) {
  const uint32_t b = (uint32_t)bank + share;
  return b > BANK_MAX_Q8 ? BANK_MAX_Q8 : b;
}

static inline uint16_t incomeFor(int cells) {
  uint32_t e = (uint32_t)cells * VIRUS_ENERGY_BASE / 64;
  return (uint16_t)(e < 1 ? 1 : e);
}

// A player's centre of mass on one axis, in sixteenths of a tile. Integer means
// would truncate a nine-tenths-of-a-tile drift to zero, which is exactly the
// motion the stall detector needs to see.
static inline uint16_t meanQ4(uint16_t sum, uint16_t cells) {
  return cells ? (uint16_t)((uint32_t)sum * 16u / cells) : 0;
}

// 160 ticks (~29 s) at 64 cells, 240 (~43 s) at 224. Deliberately not 3.5x
// longer: this is a game you WATCH, and a spectator match that runs much past
// three quarters of a minute stops being one. Of everything retuned here, this
// is the number most likely to want moving after a few real matches.
static inline uint16_t matchTicksFor(int cells) {
  return (uint16_t)(128 + (uint32_t)cells / 2);
}

// The frontier churns proportionally to its length, so a fixed one-cell wobble
// tolerance that read as "settled" at 64 cells reads as "still moving" at 224 --
// and the stalemate check would simply never fire.
static inline uint16_t stallEpsFor(int cells) {
  uint32_t e = (uint32_t)cells / 64;
  return (uint16_t)(e < 1 ? 1 : e);
}

// ---- decision bit stream ----------------------------------------------------
// Five bits per owned cell: three of action kind (six kinds), two of direction.
// No cell index travels with an entry -- both ends walk the player's owned
// cells in scan order over the same board, and position IS the index. That is
// what gets a whole board's worth of decisions into one radio frame, and it is
// also why a list applied to the wrong board is misaligned rather than merely
// stale (see NetHeader.tick).
static constexpr int DECISION_BITS = 5;

// The frame this has to fit into. A virus owning every tile is the worst case
// and it is not hypothetical -- it is what the board looks like just before a
// match ends.
static_assert((VIRUS_MAX_CELLS * DECISION_BITS + 7) / 8 <= NET_INPUT_MAX,
              "a full-board decision list does not fit NET_INPUT_MAX");

size_t Virus::decisionBytes(int cells) {
  return ((size_t)cells * DECISION_BITS + 7) / 8;
}

// Spill into the next byte whenever the 5 bits straddle a boundary, which is
// any offset past 3. Callers zero the buffer first; these only ever OR in.
static inline void putDecision(uint8_t* buf, int index, uint8_t v) {
  const int bit = index * DECISION_BITS, byte = bit >> 3, off = bit & 7;
  buf[byte] |= (uint8_t)(v << off);
  if (off > 3) buf[byte + 1] |= (uint8_t)(v >> (8 - off));
}

static inline uint8_t getDecision(const uint8_t* buf, int index) {
  const int bit = index * DECISION_BITS, byte = bit >> 3, off = bit & 7;
  uint8_t v = (uint8_t)(buf[byte] >> off);
  if (off > 3) v |= (uint8_t)(buf[byte + 1] << (8 - off));
  return v & 0x1F;
}

static inline uint8_t encodeAction(const Action& a) {
  return (uint8_t)(((uint8_t)a.kind & 0x07) | (((uint8_t)a.dir & 0x03) << 3));
}

static inline Action decodeAction(uint8_t v) {
  const uint8_t kind = v & 0x07;
  const Dir     dir  = (Dir)((v >> 3) & 0x03);
  // Only six kinds are defined. A seventh means a corrupt or hostile frame, and
  // Idle is the safe reading -- the same thing a missing list means.
  if (kind > (uint8_t)ActionKind::Spore) return Action::idle();
  return { (ActionKind)kind, dir };
}

// ---- owner-code constants --------------------------------------------------
// An owner code has to fit the 3 bits the state snapshot allots it (see
// serializeState): 0 = empty, 1..5 = a player, 6 = scorched, 7 spare.
static constexpr uint8_t OWNER_EMPTY   = 0;    // open ground
static constexpr uint8_t OWNER_NEUTRAL = 6;    // scorched ground (a cell died here)
static constexpr uint8_t CLAIM_CONTEST = 0xFE; // >=2 players grew into one tile;
                                               // scratch only, never serialized

// Neighbour offsets, indexed by Dir (N,E,S,W). y grows downward.
static const int8_t DX[4] = {  0, 1, 0, -1 };
static const int8_t DY[4] = { -1, 0, 1,  0 };

// ---- menu icon: a virus blob ----------------------------------------------
static const uint8_t VIRUS_ICON[8][8] = {
  {0,0,1,0,0,1,0,0},
  {1,0,0,1,1,0,0,1},
  {0,0,1,1,1,1,0,0},
  {0,1,1,2,2,1,1,0},
  {0,1,1,2,2,1,1,0},
  {0,0,1,1,1,1,0,0},
  {1,0,0,1,1,0,0,1},
  {0,0,1,0,0,1,0,0},
};
static const CRGB VIRUS_ICON_PAL[] = {
  CRGB::Black, CRGB(0, 200, 80), CRGB(150, 255, 150),
};
Icon Virus::menuIcon() const { return { VIRUS_ICON, VIRUS_ICON_PAL }; }

// ============================================================================
//  Lifecycle
// ============================================================================
void Virus::setRoster(const RuleFn* rules, const uint8_t* virusIdx, uint8_t n) {
  if (n > NET_MAX_PLAYERS) n = NET_MAX_PLAYERS;
  for (uint8_t i = 0; i < NET_MAX_PLAYERS; i++) {
    _rule[i] = (i < n) ? rules[i]             : nullptr;
    _tag[i]  = (i < n) ? (char)('A' + virusIdx[i]) : '?';
  }
  _localRule = nullptr;      // an explicit roster wins; see setLocalRule
}

void Virus::setLocalRule(RuleFn fn, uint8_t virusIdx) {
  _localRule = fn;
  _localIdx  = virusIdx;
}

// ============================================================================
//  Fixed openings
// ============================================================================
//  Where a virus starts is a lookup, not a roll: the same roster on the same
//  opening replays the identical match, which is the whole point -- you cannot
//  compare two rules if they were handed different boards. VirusApp and
//  Multiplayer both pass the round number to setOpening(), so a series walks
//  the openings in order and both ends of a networked match agree on the board
//  without another word crossing the wire.
//
//  Every spot is inset one tile from the wall so each seed opens with the same
//  four legal neighbours. A seed in a literal corner would start with two --
//  that is a different handicap per position rather than a difference in
//  spacing, and spacing is the only thing these are meant to vary.
//
//  Spots are listed DIAGONAL-FIRST so the first n are as far apart as the shape
//  allows: two viruses take opposite corners, never adjacent ones. Opening 4 is
//  the exception and divides the bottom wall evenly for whatever n is, since a
//  row can be split properly for any count.
//
//    0  corners                  1  wall middles      2  quadrant centres
//    3  a 2x2 block dead centre  4  evenly spaced along the bottom wall
//
//  `slot` is 0..n-1 and has already been rotated by the caller.
static void openingSpot(uint8_t opening, int slot, int n,
                        int W, int H, int& x, int& y) {
  const int L    = 1,         R    = W - 2;            // one tile in from
  const int T    = 1,         B    = H - 2;            // each wall
  const int midX = W / 2,     midY = H / 2;
  const int qx0  = W / 4,     qx1  = W - 1 - W / 4;    // quadrant centres,
  const int qy0  = H / 4,     qy1  = H - 1 - H / 4;    // symmetric about middle
  const int cx   = W / 2 - 1, cy   = H / 2 - 1;        // top-left of centre 2x2

  switch (opening) {
    case 0: {   // corners: TL, BR, TR, BL
      const int xs[4] = { L, R, R, L };
      const int ys[4] = { T, B, T, B };
      x = xs[slot]; y = ys[slot];
      break;
    }
    case 1: {   // wall middles: top, bottom, left, right
      const int xs[4] = { midX, midX, L,    R    };
      const int ys[4] = { T,    B,    midY, midY };
      x = xs[slot]; y = ys[slot];
      break;
    }
    case 2: {   // quadrant centres: TL, BR, TR, BL
      const int xs[4] = { qx0, qx1, qx1, qx0 };
      const int ys[4] = { qy0, qy1, qy0, qy1 };
      x = xs[slot]; y = ys[slot];
      break;
    }
    case 3: {   // the centre 2x2, diagonal pair first
      const int xs[4] = { cx, cx + 1, cx + 1, cx     };
      const int ys[4] = { cy, cy + 1, cy,     cy + 1 };
      x = xs[slot]; y = ys[slot];
      break;
    }
    default: {  // 4: n seeds spread evenly along the bottom wall
      x = ((2 * slot + 1) * W) / (2 * n);
      y = B;
      break;
    }
  }

  // The even split above can land on the rim of a narrow board (n=4 on a
  // 8-wide one puts the last seed at x=7, which is the wall), so every opening
  // is pinned back inside the inset box rather than trusting the arithmetic.
  if (x < L) x = L;
  if (x > R) x = R;
  if (y < T) y = T;
  if (y > B) y = B;
}

void Virus::begin(uint8_t arenaW, uint8_t arenaH, uint8_t myId,
                  uint8_t numPlayers, uint32_t seed, const CRGB* colors) {
  (void)seed;   // openings are fixed now -- nothing here is rolled
  _arenaW = arenaW; _arenaH = arenaH;
  _myId = myId; _numPlayers = numPlayers;

  // Networked: place our one rule now that the lobby has told us which player
  // we are. Everyone else's slot stays null, which is exactly what makes
  // hostTick read their actions off the wire instead of calling a rule.
  if (_localRule) {
    for (int i = 0; i < NET_MAX_PLAYERS; i++) { _rule[i] = nullptr; _tag[i] = '?'; }
    if (myId < NET_MAX_PLAYERS) {
      _rule[myId] = _localRule;
      _tag[myId]  = (char)('A' + _localIdx);
    }
  }
  _tick = 0; _phase = 0; _winner = NET_PID_NONE;
  _lastNetDelta = 0xFFFF;
  _lastDrift    = 0xFFFF;
  for (int i = 0; i < NET_MAX_PLAYERS; i++)
    _colors[i] = colors ? colors[i] : PRESET_COLORS[i % NUM_PRESET_COLORS];

  int N = (int)arenaW * arenaH;

  // Derived once from the board, so every rule below reads a number instead of
  // a formula and the whole retune is visible in one place.
  _income     = incomeFor(N);
  _matchTicks = matchTicksFor(N);
  _stallEps   = stallEpsFor(N);
  for (int i = 0; i < NET_MAX_PLAYERS; i++) _cells[i] = 0;

  memset(_owner,     OWNER_EMPTY, N);
  memset(_strength,  0,           N);
  memset(_cellBank,  0,           sizeof(_cellBank));
  memset(_netEnergy, 0,           sizeof(_netEnergy));
  for (int i = 0; i < NET_MAX_PLAYERS; i++) _pending[i].valid = false;

  // Seed one cell per player from the fixed opening, one spot each.
  //
  // WHICH virus gets which spot rotates with the opening, so over a series each
  // one plays every position. That is not decoration: the find* helpers scan
  // N,E,S,W and stop at the first hit, so a rule built on them drifts north and
  // east, and the spot with the most room that way is worth real cells. Pin a
  // virus to one corner for all five matches and part of its score is where it
  // sat. Rotating by the opening costs nothing and takes that out.
  //
  // The rotation is modulo n, not 4, so a two-virus roster swaps its two spots
  // instead of rotating onto positions nobody is using.
  const int n = (numPlayers < 4) ? (int)numPlayers : 4;
  for (int p = 0; p < n; p++) {
    const int slot = (p + (int)_opening) % n;
    int x = 0, y = 0;
    openingSpot(_opening, slot, n, arenaW, arenaH, x, y);
    const int c  = y * arenaW + x;
    _owner[c]    = (uint8_t)(p + 1);
    _strength[c] = VIRUS_START_STRENGTH;
  }
}

// ============================================================================
//  The two things a host and a client must agree on exactly.
// ============================================================================
int Virus::collectOwned(const uint8_t* owner, uint8_t code, uint16_t* out) const {
  const int N = (int)_arenaW * _arenaH;
  int n = 0;
  for (int c = 0; c < N; c++) if (owner[c] == code) out[n++] = (uint16_t)c;
  return n;
}

// Match progress, from whichever is further along -- the clock or how full the
// board is. Both inputs are shared: _tick rides in the state snapshot and the
// board is the snapshot, so a client computes the identical answer rather than
// being told it.
Phase Virus::phaseNow(const uint8_t* owner) const {
  const int N = (int)_arenaW * _arenaH;
  int empty = 0;
  for (int c = 0; c < N; c++) if (owner[c] == OWNER_EMPTY) empty++;
  uint32_t timePct = _matchTicks ? (uint32_t)_tick * 100u / _matchTicks : 0;
  uint32_t fillPct = (uint32_t)(N - empty) * 100u / (uint32_t)N;
  uint32_t prog    = (timePct > fillPct) ? timePct : fillPct;
  return (prog < 33) ? Phase::Early : (prog < 66) ? Phase::Middle : Phase::End;
}

// ============================================================================
//  Client: run MY rule over MY cells and pack what it decided.
//
//  Takes its own snapshot first, for the same reason the host does: every cell
//  must be judged against one frozen board. That board is the one the host
//  published after its last tick, which is precisely the board it will snapshot
//  at the start of the next -- so the two ends build identical Cell views and
//  the positional decision list lines up.
// ============================================================================
size_t Virus::decideLocal(uint8_t* buf, size_t cap, bool buttonA) {
  if (_myId >= NET_MAX_PLAYERS || !_rule[_myId]) return 0;

  const int N = (int)_arenaW * _arenaH;
  memcpy(_sOwner, _owner,    N);
  memcpy(_sStr,   _strength, N);

  const int n = collectOwned(_sOwner, (uint8_t)(_myId + 1), _owned);
  if (!n) return 0;

  const size_t need = decisionBytes(n);
  if (cap < need) return 0;
  memset(buf, 0, need);

  // _tick is the one the host will run next: it published this board AFTER
  // advancing, so the number adopted with the board is the number the host's
  // own rules will see. Both ends therefore hand their rules the same tick,
  // which is what makes it safe to steer a decision by it.
  World world; world.phase = phaseNow(_sOwner); world.button.held = buttonA;
  world.tick = _tick;
  for (int i = 0; i < n; i++) {
    const int c = _owned[i];
    // _netEnergy is what the host says this cell may spend -- already credited
    // and capped there. The client keeps no ledger of its own, so there is
    // nothing here that can drift out of step with the referee.
    Decision d = _rule[_myId](buildCell(c, _myId, _netEnergy[c]), world);
    putDecision(buf, i, encodeAction(d.action));
  }
  return need;
}

// ============================================================================
//  Per-cell energy, host -> the one client it belongs to.
//
//  Four bits a cell, so it has to stay inside a nibble; and a full board's
//  worth has to stay inside a frame. Both are assertions, not hopes.
// ============================================================================
static_assert(VIRUS_CELL_BANK <= 15, "a cell's bank no longer fits the 4 bits it is sent in");
static_assert((VIRUS_MAX_CELLS + 1) / 2 <= NET_PRIV_MAX,
              "a full-board energy report does not fit NET_PRIV_MAX");

size_t Virus::energyBytes(int cells) { return ((size_t)cells + 1) / 2; }

// ---- what a virus just did, in six bits ------------------------------------
// A voice only asks whether a thing happened, never how often, so the counts
// collapse to flags. Two of them -- an attack landing, a spore going out -- are
// things NO amount of comparing one board against the next can recover: a
// client can see a cell lose strength but not who hit it. That is why these
// travel rather than being worked out at the far end.
// ============================================================================
//  The tick's one sound.
//
//  Two rules, unchanged from when this lived in VirusApp:
//    1. WITHIN a virus, the loudest thing it did wins -- losing a cell outranks
//       taking one. A virus says one thing per tick.
//    2. ACROSS the slots, highest priority wins. Exactly one play() leaves here.
//
//  WHICH slots, though, is the whole difference between the two ways to play,
//  and `_tag` already draws the line without being asked to. On one device
//  setRoster() names all four viruses, so all four are audible -- it is a
//  spectator game and every virus on the board was authored right here. In a
//  networked match only this device's own slot gets a letter and the rest stay
//  '?', so a player hears their own virus and nothing else. That is the split
//  the audio design record called for, and it falls out of asking "do I know
//  what this slot is running" rather than out of a mode flag.
// ============================================================================
void Virus::speak() {
  if (!_audio) return;

  const Sfx* best = nullptr;
  for (uint8_t p = 0; p < _numPlayers && p < NET_MAX_PLAYERS; p++) {
    const char t = _tag[p];
    if (t < 'A' || t > 'D') continue;             // not ours to speak for
    const VirusVoice* v = VIRUS_VOICES[t - 'A'];
    if (!v) continue;
    const TickEvents& e = _ev[p];

    const Sfx* cand = nullptr;
    if      (e.lost     && v->onCellLost) cand = v->onCellLost;
    else if (e.spored   && v->onSpore)    cand = v->onSpore;
    else if (e.damaged  && v->onDamaged)  cand = v->onDamaged;
    else if (e.moved    && v->onMoved)    cand = v->onMoved;
    else if (e.attacked && v->onAttack)   cand = v->onAttack;
    else if (e.grew     && v->onGrow)     cand = v->onGrow;

    if (cand && (!best || cand->priority > best->priority)) best = cand;
  }

  if (best) _audio->play(*best);
}

uint8_t Virus::packEvents(uint8_t playerId) const {
  if (playerId >= NET_MAX_PLAYERS) return 0;
  const TickEvents& e = _ev[playerId];
  return (uint8_t)((e.grew     ? 0x01 : 0) | (e.attacked ? 0x02 : 0) |
                   (e.damaged  ? 0x04 : 0) | (e.lost     ? 0x08 : 0) |
                   (e.moved    ? 0x10 : 0) | (e.spored   ? 0x20 : 0));
}

void Virus::unpackEvents(uint8_t playerId, uint8_t bits) {
  if (playerId >= NET_MAX_PLAYERS) return;
  TickEvents& e = _ev[playerId];
  e.grew     = (bits & 0x01) ? 1 : 0;
  e.attacked = (bits & 0x02) ? 1 : 0;
  e.damaged  = (bits & 0x04) ? 1 : 0;
  e.lost     = (bits & 0x08) ? 1 : 0;
  e.moved    = (bits & 0x10) ? 1 : 0;
  e.spored   = (bits & 0x20) ? 1 : 0;
}

size_t Virus::serializePrivate(uint8_t playerId, uint8_t* buf, size_t cap) {
  if (playerId >= NET_MAX_PLAYERS) return 0;

  // Read the LIVE board, not the last snapshot: this describes the tick about
  // to happen, and _owner is exactly what that tick will snapshot.
  const int n = collectOwned(_owner, (uint8_t)(playerId + 1), _owned);
  if (!n) return 0;

  // Energy for the tick about to happen, then a byte of what just happened.
  // Two different moments in one frame, deliberately: they are the two things
  // this player alone needs, and they are already going to this player alone.
  const size_t need = energyBytes(n) + 1;
  if (cap < need) return 0;
  memset(buf, 0, need);

  const uint32_t share = (uint32_t)_income * ENERGY_ONE / n;
  for (int i = 0; i < n; i++) {
    const uint8_t e = (uint8_t)(creditedBank(_cellBank[_owned[i]], share) / ENERGY_ONE);
    buf[i >> 1] |= (uint8_t)((e & 0x0F) << ((i & 1) ? 4 : 0));
  }
  buf[need - 1] = packEvents(playerId);
  return need;
}

void Virus::applyPrivate(const uint8_t* buf, size_t len) {
  memset(_netEnergy, 0, sizeof(_netEnergy));
  // Cleared before the length check, so a frame that never arrives is silence
  // rather than last tick's sound played twice.
  if (_myId < NET_MAX_PLAYERS) _ev[_myId] = TickEvents{};
  if (_myId >= NET_MAX_PLAYERS) return;

  const int n = collectOwned(_owner, (uint8_t)(_myId + 1), _owned);
  // A report sized for a different cell count was computed against a different
  // board, and its nibbles line up with somebody else's cells. Drop it whole:
  // every cell then reads 0 energy and idles, which is the same safe reading a
  // missing decision list gets.
  if (!n || len != energyBytes(n) + 1) return;

  for (int i = 0; i < n; i++)
    _netEnergy[_owned[i]] = (uint8_t)((buf[i >> 1] >> ((i & 1) ? 4 : 0)) & 0x0F);

  // The host's own account of the tick just drawn. This is the client's only
  // chance to speak, so it happens here rather than in applyState: the board
  // arrives first and the events a moment later, and voicing the board alone
  // would miss the two things it cannot see.
  unpackEvents(_myId, buf[len - 1]);
  speak();
}

void Virus::applyInput(uint8_t playerId, const uint8_t* buf, size_t len) {
  if (playerId >= NET_MAX_PLAYERS) return;
  uint8_t n = len > NET_INPUT_MAX ? NET_INPUT_MAX : (uint8_t)len;
  memcpy(_pending[playerId].buf, buf, n);
  _pending[playerId].len   = n;
  _pending[playerId].valid = (n > 0);
}

// ============================================================================
//  Snapshot view handed to a rule for one of its cells.
// ============================================================================
Cell Virus::buildCell(int c, uint8_t player, uint16_t energy) const {
  Cell me;
  me.strength = (Strength)_sStr[c];   // internal 0..4 maps straight onto the enum
  me.energy   = energy;               // whole units out of this cell's own purse
  me.note     = 0;
  int cx = c % _arenaW, cy = c / _arenaW;
  for (int d = 0; d < 4; d++) {
    // Spore sensing: is the tile VIRUS_SPORE_REACH steps this way on the board
    // and open? Only the landing tile matters -- a spore leaps what lies between.
    int fx = cx + VIRUS_SPORE_REACH * DX[d], fy = cy + VIRUS_SPORE_REACH * DY[d];
    if (fx < 0 || fx >= _arenaW || fy < 0 || fy >= _arenaH) {
      me.far_open[d] = false;
    } else {
      uint8_t fo = _sOwner[fy * _arenaW + fx];
      me.far_open[d] = (fo == OWNER_EMPTY || fo == OWNER_NEUTRAL);
    }

    Neighbour& nb = me.n[d];
    nb.strength = Strength::None; nb.enemy_id = 0; nb.note = 0;
    int nx = cx + DX[d], ny = cy + DY[d];
    if (nx < 0 || nx >= _arenaW || ny < 0 || ny >= _arenaH) {
      nb.occupant = Occupant::Edge;
      continue;
    }
    int nc = ny * _arenaW + nx;
    uint8_t o = _sOwner[nc];
    if (o == OWNER_EMPTY) {
      nb.occupant = Occupant::Empty;
    } else if (o == OWNER_NEUTRAL) {
      nb.occupant = Occupant::Neutral;
    } else if (o == player + 1) {
      nb.occupant = Occupant::Self;  nb.strength = (Strength)_sStr[nc];
    } else {
      nb.occupant = Occupant::Enemy; nb.strength = (Strength)_sStr[nc]; nb.enemy_id = o;
    }
  }
  return me;
}

// ============================================================================
//  Apply one funded action to the tick's accumulators, if it is legal against
//  the snapshot. Illegal actions are dropped; energy is NOT refunded (already
//  spent by the caller).
// ============================================================================
void Virus::applyIfLegal(uint8_t player, int c, const Action& a) {
  int cx = c % _arenaW, cy = c / _arenaW;

  switch (a.kind) {
    case ActionKind::Fortify:
      if (_sStr[c] < VIRUS_STRENGTH_MAX) _fort[c] = 1;
      break;

    // Three ways to put a cell on an open tile, and they all resolve through
    // one claim map, so any two of them racing for the same tile contest each
    // other exactly as you would expect:
    //   Grow  claims the adjacent tile and puts a NEW cell there.
    //   Move  claims the adjacent tile and walks THIS cell into it.
    //   Spore claims the tile VIRUS_SPORE_REACH steps away, ignoring whatever
    //         sits between (what the extra energy buys).
    case ActionKind::Grow:
    case ActionKind::Move:
    case ActionKind::Spore: {
      int reach = (a.kind == ActionKind::Spore) ? VIRUS_SPORE_REACH : 1;
      int d  = (int)a.dir;
      int nx = cx + reach * DX[d], ny = cy + reach * DY[d];
      if (nx < 0 || nx >= _arenaW || ny < 0 || ny >= _arenaH) break;   // off board
      int nc = ny * _arenaW + nx;
      uint8_t o = _sOwner[nc];
      if (o != OWNER_EMPTY && o != OWNER_NEUTRAL) break;               // occupied
      if (_claimBy[nc] == 0) {
        _claimBy[nc] = player + 1;
        // Only a Move records a source, and only when it takes the claim
        // outright. If some earlier cell of yours already claimed this tile,
        // the mover simply stays where it is -- it must never vacate for a
        // claim it did not win, or the cell would vanish.
        if (a.kind == ActionKind::Move) _claimSrc[nc] = (uint16_t)c;
      } else if (_claimBy[nc] != player + 1) {
        _claimBy[nc] = CLAIM_CONTEST;
      }
      // Audio only. A Spore is announced on the attempt rather than the landing:
      // it costs six energy and is the one deliberate, rare act in the game, so
      // it is worth hearing even when another virus contests the tile away.
      // Grow and Move are announced in commitTick(), where they either land or
      // do not -- they are common enough that attempts would be noise.
      if (a.kind == ActionKind::Spore) bump(_ev[player].spored);
      break;
    }

    case ActionKind::Attack: {
      int d = (int)a.dir;
      int nx = cx + DX[d], ny = cy + DY[d];
      if (nx < 0 || nx >= _arenaW || ny < 0 || ny >= _arenaH) break;   // Edge
      int nc = ny * _arenaW + nx;
      uint8_t o = _sOwner[nc];
      if (o == OWNER_EMPTY || o == OWNER_NEUTRAL || o == player + 1) break; // not enemy
      _damage[nc] += 1;
      bump(_ev[player].attacked);      // audio only
      break;
    }

    default: break;   // Idle
  }
}

// ============================================================================
//  Authoritative tick (spec §3): snapshot -> decide/validate per cell ->
//  combat/claims/commit for everyone at once.
// ============================================================================
void Virus::hostTick() {
  if (_phase != 0) return;

  const int N = (int)_arenaW * _arenaH;
  memcpy(_sOwner, _owner,    N);
  memcpy(_sStr,   _strength, N);
  memset(_damage,   0, sizeof(uint16_t) * N);
  memset(_fort,     0, N);
  memset(_claimBy,  0, N);
  memset(_claimSrc, 0xFF, sizeof(uint16_t) * N);   // 0xFFFF == CLAIM_NONE
  memset(_ev,       0, sizeof(_ev));   // render-only event counts, rebuilt per tick

  // Match phase = the larger of "time elapsed" and "board filled", as integer
  // percentages (no floating point, for determinism). Empty tiles only shrink
  // over a match, so board-fill rises monotonically; taking the max of the two
  // makes a fast-saturating game reach End early, tracking the real pace.
  // _tick is still this tick's number here -- it is not advanced until the end
  // of hostTick -- which is exactly the value a client was handed with the
  // board it is deciding against. See the matching note in serializeInput.
  World world; world.phase = phaseNow(_sOwner); world.button.held = _buttonA;
  world.tick = _tick;

  uint8_t req[NET_MAX_PLAYERS]  = { 0 };  // non-idle actions a player wanted
  uint8_t fund[NET_MAX_PLAYERS] = { 0 };  // how many the cells could pay for
  // Where each player's actions came from this tick. Telemetry only, but the
  // one thing act=x/y cannot tell you over the air: a virus doing nothing
  // because its rule chose to, and a virus doing nothing because its device
  // never answered, look identical in the counts and are entirely different
  // problems. '.' local rule, '+' decisions arrived, '-' nothing heard.
  char src[NET_MAX_PLAYERS];
  for (uint8_t p = 0; p < NET_MAX_PLAYERS; p++) src[p] = ' ';

  for (uint8_t p = 0; p < _numPlayers; p++) {
    // This tick's income, split evenly across the cells owned AT SNAPSHOT.
    // Cells claimed during this tick earn nothing until the next one -- they do
    // not exist yet as far as the split is concerned.
    const int owned = collectOwned(_sOwner, (uint8_t)(p + 1), _owned);
    if (!owned) { _pending[p].valid = false; continue; }
    const uint32_t share = (uint32_t)_income * ENERGY_ONE / owned;

    // Where this player's decisions come from. A local rule is called; a remote
    // one already decided, on its own device, against this very board -- the
    // referee cannot tell the difference and does not try. A player with
    // neither idles, which is what a disconnected unit does and also what a
    // dropped or mistagged decision list comes to.
    // A list is sized by the sender's cell count, so a length that does not
    // match ours proves it was computed against a different board however the
    // tick tag looked. Second, cheap, independent check on the same invariant.
    RuleFn        rule = _rule[p];
    const Pending& pen = _pending[p];
    const bool     net = !rule && pen.valid && pen.len == decisionBytes(owned);
    src[p] = rule ? '.' : (net ? '+' : '-');

    // Decide and pay, one cell at a time, in the ONE agreed order. There is no
    // funding walk: a cell's purse is its own, so whether it can act depends on
    // nothing but its own bank, and an unaffordable request costs its owner
    // nothing except this cell's tick. me.canAfford*() is a promise, not a hint.
    uint16_t wanted = 0, funded = 0;
    for (int i = 0; i < owned; i++) {
      const int c = _owned[i];

      uint32_t bank = creditedBank(_cellBank[c], share);

      Action a = Action::idle();
      if (rule)     a = rule(buildCell(c, p, (uint16_t)(bank / ENERGY_ONE)), world).action;
      else if (net) a = decodeAction(getDecision(pen.buf, i));

      const uint32_t cost = (uint32_t)a.cost() * ENERGY_ONE;
      if (cost) {
        wanted++;
        if (cost <= bank) {
          bank -= cost;                  // spent, even if it proves illegal below
          applyIfLegal(p, c, a);
          funded++;
        }
      }
      _cellBank[c] = (uint16_t)bank;     // bank the remainder for next tick
    }

    // Consumed. A list is good for exactly the tick it was computed against, so
    // it must never survive into the next one.
    _pending[p].valid = false;
    req[p]  = (wanted > 255) ? 255 : (uint8_t)wanted;
    fund[p] = (funded > 255) ? 255 : (uint8_t)funded;
  }

  commitTick();       // combat, claims, commit
  _tickWallMs = millis();   // render-only: start of this tick's combat flashes
  _tick++;
  evaluateEnd();       // scoring + stalemate detection (per-player count window)
  speak();             // one sound for the tick, from the events it produced

#if VIRUS_TELEMETRY
  // One line per tick: each virus's cells, total strength, banked energy across
  // all its cells, and funded/wanted actions. "act=1/4" means four of its cells
  // asked to do something and one of them could pay for it.
  {
    uint16_t cN[NET_MAX_PLAYERS] = { 0 };
    uint32_t sN[NET_MAX_PLAYERS] = { 0 };
    uint32_t bN[NET_MAX_PLAYERS] = { 0 };
    for (int c = 0; c < N; c++) {
      uint8_t o = _owner[c];
      if (o >= 1 && o <= _numPlayers) {
        cN[o - 1]++;
        sN[o - 1] += _strength[c];
        bN[o - 1] += _cellBank[c];
      }
    }
    Serial.printf("[virus] t=%3u", _tick);
    for (uint8_t p = 0; p < _numPlayers; p++)
      Serial.printf(" | %c%c cells=%2u str=%3lu bank=%3lu act=%u/%u",
                    _tag[p], src[p], cN[p], (unsigned long)sN[p],
                    (unsigned long)(bN[p] / ENERGY_ONE), fund[p], req[p]);
    if (_lastNetDelta != 0xFFFF)
      Serial.printf("  netD=%u/%u drift=%u/%u",
                    _lastNetDelta, _stallEps, _lastDrift, VIRUS_STALL_DRIFT_Q4);
    if (_phase) {
      if (_winner == NET_PID_NONE)          Serial.print("  OVER draw");
      else if (_winner < NET_MAX_PLAYERS)   Serial.printf("  OVER winner=%c", _tag[_winner]);
    }
    Serial.println();
  }
#endif
}

// ============================================================================
//  Combat, claims, commit. All read the snapshot; all write the live board.
//
//  Three passes, because Move makes the old single pass impossible: whether a
//  cell lands on the tile it walked into depends on whether it survived the
//  combat happening at the tile it walked out of, and a single pass over the
//  board would reach the two tiles in either order.
//
//    A  fates    -- who died, and what everyone's strength becomes
//    B  claims   -- tiles that were open: who, if anyone, takes them (a Move
//                   claim reads pass A's verdict on its source, and marks that
//                   source vacated)
//    C  occupied -- tiles that were owned: died / vacated / stayed
//
//  B and C write disjoint sets of tiles, but B READS state that C overwrites
//  (the mover's strength and purse), so B has to run first.
// ============================================================================
void Virus::commitTick() {
  const int N = (int)_arenaW * _arenaH;
  memset(_fx,      0, N);      // render-only: rebuilt from scratch each tick
  memset(_vacated, 0, N);

  // ---- Pass A: fates ------------------------------------------------------
  // Damage is measured against SNAPSHOT strength, so a Fortify this tick cannot
  // save a cell from lethal damage.
  for (int c = 0; c < N; c++) {
    uint8_t o = _sOwner[c];
    if (o == OWNER_EMPTY || o == OWNER_NEUTRAL) { _died[c] = 0; _newStr[c] = 0; continue; }
    if (_damage[c] >= _sStr[c]) {
      _died[c] = 1; _newStr[c] = 0;
    } else {
      int ns = (int)_sStr[c] - _damage[c] + _fort[c];
      if (ns > VIRUS_STRENGTH_MAX) ns = VIRUS_STRENGTH_MAX;
      _died[c] = 0; _newStr[c] = (uint8_t)ns;
    }
  }

  // ---- Pass B: claims onto ground that was open ---------------------------
  // A single claimant takes it; two or more annihilate (tile unchanged,
  // everyone already paid).
  for (int c = 0; c < N; c++) {
    uint8_t o = _sOwner[c];
    if (o != OWNER_EMPTY && o != OWNER_NEUTRAL) continue;

    uint8_t  cl  = _claimBy[c];
    uint16_t src = _claimSrc[c];

    if (cl == 0 || cl == CLAIM_CONTEST) {
      _owner[c] = o; _strength[c] = 0; _cellBank[c] = 0;    // stays Empty/Neutral
      continue;
    }

    if (src == CLAIM_NONE) {                                // Grow or Spore
      _owner[c] = cl; _strength[c] = VIRUS_GROW_STRENGTH; _cellBank[c] = 0;
      bump(_ev[cl - 1].grew);        // audio only: the claim actually landed
      continue;
    }

    // A Move. If the mover was killed where it stood it never arrives, and the
    // tile it was walking into stays open -- attackers hit it where it was.
    if (_died[src]) { _owner[c] = o; _strength[c] = 0; _cellBank[c] = 0; continue; }

    _owner[c]    = cl;
    _strength[c] = _newStr[src];       // it is the same cell, carried over
    _cellBank[c] = _cellBank[src];     // purse and all
    _vacated[src] = 1;
    bump(_ev[cl - 1].moved);           // audio only
    // A cell that was hit on its way out still shows the hit, at the tile it
    // ended the tick on -- otherwise the flash would land on empty ground.
    if (_damage[src] > 0) { _fx[c] = FX_DAMAGED; bump(_ev[cl - 1].damaged); }
  }

  // ---- Pass C: tiles that were occupied -----------------------------------
  for (int c = 0; c < N; c++) {
    uint8_t o = _sOwner[c];
    if (o == OWNER_EMPTY || o == OWNER_NEUTRAL) continue;

    if (_died[c]) {
      _owner[c] = OWNER_NEUTRAL; _strength[c] = 0; _cellBank[c] = 0;   // killed -> scorched
      _fx[c] = FX_DIED;
      bump(_ev[o - 1].lost);                                           // audio only
    } else if (_vacated[c]) {
      _owner[c] = OWNER_EMPTY;  _strength[c] = 0; _cellBank[c] = 0;    // walked away
    } else {
      _owner[c] = o; _strength[c] = _newStr[c];
      if (_damage[c] > 0) { _fx[c] = FX_DAMAGED; bump(_ev[o - 1].damaged); }
    }
  }
}

// ============================================================================
//  End conditions (spec §9): last player standing ends immediately; otherwise
//  the match runs to _matchTicks and scores on cells, then strength.
// ============================================================================
void Virus::evaluateEnd() {
  const int N = (int)_arenaW * _arenaH;
  uint16_t cells[NET_MAX_PLAYERS] = { 0 };
  uint32_t str[NET_MAX_PLAYERS]   = { 0 };
  uint16_t sumX[NET_MAX_PLAYERS]  = { 0 };   // for the centroid; 224 * 15 fits
  uint16_t sumY[NET_MAX_PLAYERS]  = { 0 };
  for (int c = 0; c < N; c++) {
    uint8_t o = _owner[c];
    if (o >= 1 && o <= _numPlayers) {
      cells[o - 1]++;
      str[o - 1]  += _strength[c];
      sumX[o - 1] += (uint16_t)(c % _arenaW);
      sumY[o - 1] += (uint16_t)(c / _arenaW);
    }
  }

  // Cache for the live territory bar. Counted here anyway, every tick, so the
  // HUD costs nothing beyond the copy -- and reading it back out of the sim is
  // strictly better than the app re-scanning 224 cells to draw one row.
  for (uint8_t p = 0; p < NET_MAX_PLAYERS; p++) _cells[p] = cells[p];

  int alive = 0, last = -1;
  for (uint8_t p = 0; p < _numPlayers; p++) if (cells[p] > 0) { alive++; last = p; }
  if (_numPlayers >= 2 && alive <= 1) {
    _phase  = 1;
    _winner = (alive == 1) ? (uint8_t)last : NET_PID_NONE;
    return;
  }

  // Stalemate: compare each player's cell count AND centre of mass against
  // their values one full window ago (the ring slot about to be overwritten).
  // A flickering seam and a frozen one both look flat on count -- that is the
  // point of measuring net territory rather than the owner map tick to tick.
  // But cells that Move change no count at all, so count alone would call a
  // swarm gliding across the board stalled. The centroid is the second opinion:
  // a swarm going somewhere keeps playing, a pair oscillating in place (whose
  // centroid is just as flat as a frozen one's) does not.
  bool stalled = false;
  uint16_t slot = _tick % VIRUS_STALL_WINDOW;
  if (_tick > VIRUS_STALL_WINDOW) {
    uint16_t maxDelta = 0, maxDrift = 0;
    for (uint8_t p = 0; p < _numPlayers; p++) {
      int d = (int)cells[p] - (int)_cntHist[slot][p];
      if (d < 0) d = -d;
      if ((uint16_t)d > maxDelta) maxDelta = (uint16_t)d;

      // A player with no cells now or then has no centroid to compare.
      if (!cells[p] || !_cntHist[slot][p]) continue;
      int dx = (int)meanQ4(sumX[p], cells[p]) - (int)meanQ4(_sxHist[slot][p], _cntHist[slot][p]);
      int dy = (int)meanQ4(sumY[p], cells[p]) - (int)meanQ4(_syHist[slot][p], _cntHist[slot][p]);
      if (dx < 0) dx = -dx;
      if (dy < 0) dy = -dy;
      if ((uint16_t)dx > maxDrift) maxDrift = (uint16_t)dx;
      if ((uint16_t)dy > maxDrift) maxDrift = (uint16_t)dy;
    }
    _lastNetDelta = maxDelta;
    _lastDrift    = maxDrift;
    stalled = (maxDelta <= _stallEps) && (maxDrift < VIRUS_STALL_DRIFT_Q4);
  } else {
    _lastNetDelta = 0xFFFF;   // window not full yet
    _lastDrift    = 0xFFFF;
  }
  for (uint8_t p = 0; p < _numPlayers; p++) {
    _cntHist[slot][p] = cells[p];
    _sxHist [slot][p] = sumX[p];
    _syHist [slot][p] = sumY[p];
  }

  if (_tick >= _matchTicks || stalled) {
    _phase = 1;
    int      bestP = -1;
    uint16_t bestC = 0;
    uint32_t bestS = 0;
    bool     tie   = false;
    for (uint8_t p = 0; p < _numPlayers; p++) {
      if (cells[p] > bestC || (cells[p] == bestC && str[p] > bestS)) {
        bestP = p; bestC = cells[p]; bestS = str[p]; tie = false;
      } else if (cells[p] == bestC && str[p] == bestS) {
        tie = true;                       // exact draw with the current leader
      }
    }
    _winner = (bestP >= 0 && !tie) ? (uint8_t)bestP : NET_PID_NONE;
  }
}

// ============================================================================
//  Render: owner -> player color, strength -> brightness. Neutral is a dim
//  gray; Empty is off. On top of that, cells that took damage on the last tick
//  pulse red and cells that died pulse white, both fading out over
//  VIRUS_FX_MS. render() runs every main-loop pass (~5 ms) while the sim ticks
//  every VIRUS_TICK_MS, so there are plenty of frames to fade across.
// ============================================================================
void Virus::render(Display& disp) const {
  // Strength -> brightness. Spread wide so all four tiers are distinguishable
  // at the panel's low global brightness; this is what makes fortifying and
  // being worn down readable tick over tick.
  static const uint8_t LEVEL[5] = { 0, 35, 100, 180, 255 };

  // Flash strength: 255 right after a tick, fading to 0 across VIRUS_FX_MS.
  uint32_t since = millis() - _tickWallMs;
  uint8_t  fxAmt = (since < VIRUS_FX_MS)
                 ? (uint8_t)(255u - since * 255u / VIRUS_FX_MS) : 0;

  disp.clear();
  for (int y = 0; y < _arenaH; y++)
    for (int x = 0; x < _arenaW; x++) {
      int     c  = y * _arenaW + x;
      uint8_t o  = _owner[c];
      uint8_t fx = _fx[c];

      if (o == OWNER_EMPTY) continue;

      if (o == OWNER_NEUTRAL) {
        CRGB col(16, 16, 16);                        // scorched ground
        if (fxAmt && (fx & FX_DIED))                 // a kill: white pulse, fading to gray
          col = blend(col, CRGB(255, 255, 255), fxAmt);
        disp.setPixel(x, y, col);
        continue;
      }

      uint8_t s = _strength[c];
      if (s < 1) s = 1; else if (s > 4) s = 4;
      CRGB col = _colors[o - 1];
      col.nscale8(LEVEL[s]);
      if (fxAmt && (fx & FX_DAMAGED))                // took a hit and lived: red pulse
        col = blend(col, CRGB(255, 0, 0), fxAmt);
      disp.setPixel(x, y, col);
    }

  if (_phase != 0 && _winner != NET_PID_NONE)
    disp.border(_colors[_winner]);

  drawHud(disp);   // last: the clock row sits on the border's bottom edge
}

// ============================================================================
//  HUD -- the two rows the arena gave up for it.
//
//  Row _arenaH:     the width split in proportion to who owns what, in each
//                   virus's colour. Unclaimed ground is left dark, so the bar
//                   reads as "how much of the board is spoken for" as well as
//                   "by whom".
//  Row _arenaH + 1: how much of the match clock is gone.
//
//  Drawn by the GAME, not by a runner. Virus is what shortened its own arena to
//  make room, so it is what should fill the space -- and there are two runners
//  now. This lived in VirusApp until Virus went networked, at which point a
//  networked match had two dead rows and no obvious owner for them.
// ============================================================================
void Virus::drawHud(Display& disp) const {
  const int tRow = _arenaH, cRow = _arenaH + 1;

  uint32_t total = 0;
  for (uint8_t p = 0; p < _numPlayers && p < NET_MAX_PLAYERS; p++) total += _cells[p];
  if (total) {
    int x = 0;
    for (uint8_t p = 0; p < _numPlayers && p < NET_MAX_PLAYERS && x < _arenaW; p++) {
      // Last player takes the remainder, so rounding never leaves a stray gap.
      int w = (p == _numPlayers - 1) ? _arenaW - x
                                     : (int)((uint32_t)_cells[p] * _arenaW / total);
      for (int k = 0; k < w && x < _arenaW; k++, x++)
        disp.setPixel(x, tRow, _colors[p]);
    }
  }

  const int gone = _matchTicks ? (int)((uint32_t)_tick * _arenaW / _matchTicks) : 0;
  for (int i = 0; i < _arenaW; i++)
    disp.setPixel(i, cRow, i < gone ? CRGB(40, 40, 40) : CRGB(0, 90, 110));
}

// ============================================================================
//  State (host-produced, client-consumed).
//
//  Six bits per cell -- a 3-bit owner code and a 3-bit strength -- packed four
//  cells into three bytes behind a 7-byte header. That is 55 bytes at 8x8 and
//  199 at 16x16, both inside NET_STATE_MAX (240, the most one ESP-NOW frame
//  carries after the 10-byte header).
//
//  The obvious byte-per-cell layout does not survive the bigger panel: 16x16
//  would be 261 bytes, over the wire cap outright. Packing is what makes Virus
//  multiplayer possible at 16x16, which is why the owner code is capped at 3
//  bits and OWNER_NEUTRAL is 6 rather than a nibble's 15.
//
//  Cell byte layout, four cells (q0..q3) across three bytes:
//    byte0 = q0[5:0]              | q1[1:0] << 6
//    byte1 = q1[5:2]              | q2[3:0] << 4
//    byte2 = q2[5:4]              | q3[5:0] << 2
// ============================================================================
size_t Virus::serializeState(uint8_t* buf, size_t cap) {
  const int    N    = (int)_arenaW * _arenaH;
  const size_t need = VIRUS_STATE_HDR + ((size_t)N + 3) / 4 * 3;   // 3 bytes per 4 cells
  if (cap < need) return 0;

  size_t o = 0;
  buf[o++] = _phase;
  buf[o++] = _winner;
  buf[o++] = _numPlayers;
  buf[o++] = _arenaW;
  buf[o++] = _arenaH;
  // The tick costs two bytes and buys the client an identical Phase instead of
  // an approximated one: phaseNow() blends elapsed time with board fill, and
  // the board it already has. Sending the answer would work too, but a client
  // that derives it cannot drift from a host that derives it the same way.
  buf[o++] = (uint8_t)(_tick & 0xFF);
  buf[o++] = (uint8_t)(_tick >> 8);

  for (int c = 0; c < N; c += 4) {
    uint8_t q[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < 4 && c + k < N; k++)
      q[k] = (uint8_t)((_owner[c + k] & 0x07) | ((_strength[c + k] & 0x07) << 3));
    buf[o++] = (uint8_t)( q[0]        | (q[1] << 6));
    buf[o++] = (uint8_t)((q[1] >> 2)  | (q[2] << 4));
    buf[o++] = (uint8_t)((q[2] >> 4)  | (q[3] << 2));
  }
  return o;
}

void Virus::applyState(const uint8_t* buf, size_t len) {
  if (len < VIRUS_STATE_HDR) return;
  size_t o = 0;
  _phase      = buf[o++];
  _winner     = buf[o++];
  _numPlayers = buf[o++];
  _arenaW     = buf[o++];
  _arenaH     = buf[o++];
  const uint16_t prevTick = _tick;
  _tick       = (uint16_t)(buf[o] | (buf[o + 1] << 8)); o += 2;

  // A board that has actually moved on is a new tick's worth of combat to draw.
  // The same board arriving twice is a retransmit, and rebuilding the flashes
  // off it would restart every pulse and leave them lit far longer than the
  // 60 ms they are supposed to last.
  const bool advanced = (_tick != prevTick);

  // Arena dimensions arrive off the wire, so a corrupt or mismatched frame
  // could otherwise index past the boards.
  const int N = (int)_arenaW * _arenaH;
  if (N <= 0 || N > VIRUS_MAX_CELLS) return;

  for (int c = 0; c < N; c += 4) {
    if (o + 3 > len) break;                    // truncated frame: keep what arrived
    const uint8_t b0 = buf[o++], b1 = buf[o++], b2 = buf[o++];
    const uint8_t q[4] = {
      (uint8_t)(  b0        & 0x3F),
      (uint8_t)(((b0 >> 6)  & 0x03) | ((b1 & 0x0F) << 2)),
      (uint8_t)(((b1 >> 4)  & 0x0F) | ((b2 & 0x03) << 4)),
      (uint8_t)( (b2 >> 2)  & 0x3F),
    };
    for (int k = 0; k < 4 && c + k < N; k++) {
      const int     cc = c + k;
      const uint8_t po = _owner[cc], ps = _strength[cc];      // the board this replaces
      const uint8_t no = (uint8_t)( q[k]       & 0x07);
      const uint8_t ns = (uint8_t)((q[k] >> 3) & 0x07);
      _owner[cc]    = no;
      _strength[cc] = ns;

      // The combat flashes, recovered by comparing the two boards. A client
      // never runs commitTick(), where the host fills these in, so without this
      // the panel simply never pulses -- the same hole the cell counts below
      // used to have. Both are exact: a cell of somebody's that turned to
      // scorched ground was killed, and one that kept its owner and lost
      // strength was hit and lived. Nothing here needs to know WHO did it,
      // which is precisely why the flashes can be derived and the voices cannot.
      if (!advanced) continue;
      if      (po >= 1 && po <= 4 && no == OWNER_NEUTRAL) _fx[cc] = FX_DIED;
      else if (po == no && po >= 1 && po <= 4 && ns < ps) _fx[cc] = FX_DAMAGED;
      else                                                _fx[cc] = 0;
    }
  }

  // The territory bar reads these, and the host fills them in evaluateEnd(),
  // which a client never runs -- so on a client they would have stayed at the
  // zero begin() left and the bar would simply never have appeared. Recount
  // from the board just adopted: 224 cells at 5.5 Hz costs nothing, and it
  // means both roles draw the same HUD from the same numbers.
  for (int i = 0; i < NET_MAX_PLAYERS; i++) _cells[i] = 0;
  for (int c = 0; c < N; c++) {
    const uint8_t o = _owner[c];
    if (o >= 1 && o <= _numPlayers && o <= NET_MAX_PLAYERS) _cells[o - 1]++;
  }

  // Starts the fade on the flashes just derived. render() measures from here,
  // so without it every pulse would be stuck at whatever age the host's last
  // local tick left behind.
  if (advanced) _tickWallMs = millis();
}
