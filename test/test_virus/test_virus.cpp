// ============================================================================
//  Native tests for the Virus referee (src/games/virus/virus.cpp).
//
//  These pin the mechanics a virus author is told about in virus_rules.cpp --
//  the ones that are easy to break by accident and invisible until a match
//  goes strangely: whose energy is whose, simultaneous resolution, what a kill
//  actually leaves behind, and the handful of ways a Move can fail without
//  costing you the cell.
//
//  Run with:  pio test -e native
//
//  Boards are built with applyState() and read back with serializeState(),
//  which is how a client sees the match, so the tests need no access to the
//  referee's internals. Six bits per cell -- 3-bit owner, 3-bit strength --
//  packed four cells into three bytes behind a 5-byte header.
//
//  ENERGY, for reading the setups below. On this 8x8 board a virus earns 4 per
//  tick, split evenly across the cells it owns and banked per cell in 1/256ths:
//  one cell earns 4 a tick, two earn 2 each, four earn 1 each. A cell spends
//  only from its own purse, so several tests place filler cells purely to
//  divide the income down to a rate that makes the banking visible.
//
//  Those setups are calibrated against that income, so the two tests that count
//  ticks call requireIncome() first -- sweep VIRUS_ENERGY_BASE and they will say
//  so plainly instead of failing somewhere confusing.
// ============================================================================
#include <unity.h>
#include <string.h>
#include "games/virus/virus.h"
#include "net/net_proto.h"

static const int W = 8, H = 8, N = W * H;

static const uint8_t OWNER_EMPTY   = 0;
static const uint8_t OWNER_NEUTRAL = 6;

// Bytes a snapshot of `cells` cells occupies: the header, then three bytes for
// every four cells.
static size_t stateBytes(int cells) {
  return VIRUS_STATE_HDR + ((size_t)cells + 3) / 4 * 3;
}

// ---- board helpers ---------------------------------------------------------

struct Scenario {
  Virus   game;
  uint8_t owner[N];
  uint8_t str[N];

  // Place a cell before load(). player 0 = empty, 1..4 = a virus.
  void put(int x, int y, uint8_t player, uint8_t strength) {
    owner[y * W + x] = player;
    str[y * W + x]   = strength;
  }

  void clear() { memset(owner, 0, N); memset(str, 0, N); }

  // Roster first, then the board. begin() seeds its own starting cells; load()
  // overwrites the whole board, so those seeds never survive into a test.
  void start(const RuleFn* rules, uint8_t numPlayers) {
    uint8_t idx[4] = { 0, 1, 2, 3 };
    game.setRoster(rules, idx, numPlayers);
    game.begin(W, H, 0, numPlayers, 0xC0FFEE, nullptr);
  }

  void load(uint8_t numPlayers) {
    uint8_t buf[512];
    size_t  o = 0;
    buf[o++] = 0;            // phase: running
    buf[o++] = 0xFF;         // winner: none
    buf[o++] = numPlayers;
    buf[o++] = W;
    buf[o++] = H;
    buf[o++] = 0; buf[o++] = 0;   // tick
    for (int c = 0; c < N; c += 4) {
      uint8_t q[4] = { 0, 0, 0, 0 };
      for (int k = 0; k < 4 && c + k < N; k++)
        q[k] = (uint8_t)((owner[c + k] & 0x07) | ((str[c + k] & 0x07) << 3));
      buf[o++] = (uint8_t)( q[0]       | (q[1] << 6));
      buf[o++] = (uint8_t)((q[1] >> 2) | (q[2] << 4));
      buf[o++] = (uint8_t)((q[2] >> 4) | (q[3] << 2));
    }
    TEST_ASSERT_EQUAL_size_t(stateBytes(N), o);
    game.applyState(buf, o);
  }

  void tick(int n = 1) { for (int i = 0; i < n; i++) game.hostTick(); }

  void read() {
    uint8_t buf[512];
    size_t  len = game.serializeState(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(stateBytes(N), len);
    size_t o = VIRUS_STATE_HDR;
    for (int c = 0; c < N; c += 4) {
      const uint8_t b0 = buf[o++], b1 = buf[o++], b2 = buf[o++];
      const uint8_t q[4] = {
        (uint8_t)(  b0       & 0x3F),
        (uint8_t)(((b0 >> 6) & 0x03) | ((b1 & 0x0F) << 2)),
        (uint8_t)(((b1 >> 4) & 0x0F) | ((b2 & 0x03) << 4)),
        (uint8_t)( (b2 >> 2) & 0x3F),
      };
      for (int k = 0; k < 4 && c + k < N; k++) {
        owner[c + k] = (uint8_t)( q[k]       & 0x07);
        str[c + k]   = (uint8_t)((q[k] >> 3) & 0x07);
      }
    }
  }

  uint8_t ownerAt(int x, int y) const { return owner[y * W + x]; }
  uint8_t strAt(int x, int y)   const { return str[y * W + x];   }

  int cellCount(uint8_t player) const {
    int n = 0;
    for (int c = 0; c < N; c++) if (owner[c] == player) n++;
    return n;
  }

  bool isOver() const { uint8_t w; return game.isOver(w); }

  // Tests that count ticks depend on the exact per-tick income; say so.
  void requireIncome(uint16_t want) const {
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(
      want, game.income(),
      "VIRUS_ENERGY_BASE moved -- this test's tick counts need recalibrating");
  }
};

// ---- rules under test ------------------------------------------------------
// A rule is pure and sees only its own cell, so a test tells two cells apart by
// giving them different strengths and branching on me.strength.

static Decision ruleIdle(const Cell&, const World&) {
  return Action::idle();
}

static Decision ruleGrowNorth(const Cell&, const World&) {
  return Action::grow(Dir::N);
}

static Decision ruleGrowEast(const Cell&, const World&) {
  return Action::grow(Dir::E);
}

static Decision ruleGrowWest(const Cell&, const World&) {
  return Action::grow(Dir::W);
}

static Decision ruleFortify(const Cell& me, const World&) {
  if (me.canAffordFortify()) return Action::fortify();
  return Action::idle();
}

static Decision ruleAttack(const Cell& me, const World&) {
  Dir d = Dir::N;
  if (me.findWeakestEnemy(d)) return Action::attack(d);
  return Action::idle();
}

static Decision ruleMoveEast(const Cell&, const World&) {
  return Action::move(Dir::E);
}

// Walk east while there is anywhere to walk to, then stop.
static Decision ruleWalkEast(const Cell& me, const World&) {
  if (me.canMove(Dir::E)) return Action::move(Dir::E);
  return Action::idle();
}

// The Strongest cell asks for a Spore it cannot pay for; everyone else asks to
// Grow. In v1's shared purse the Spore came first and starved the Grow -- with
// per-cell purses the two cells cannot touch each other's money at all.
static Decision ruleSporeOrGrow(const Cell& me, const World&) {
  if (me.strength == Strength::Strongest) return Action::spore(Dir::S);
  return Action::grow(Dir::N);
}

// Only the Strongest cell does anything: save up, then leap the blockade east.
static Decision ruleSporeEastWhenStrongest(const Cell& me, const World&) {
  if (me.strength != Strength::Strongest)         return Action::idle();
  if (me.canSpore(Dir::E))   // canSpore now covers the purse as well as the tile
    return Action::spore(Dir::E);
  return Action::idle();
}

// Bank until a Spore is affordable, take one step east off the west rim, then
// spore from where it lands. Only a purse that travels with the cell makes the
// spore possible on the tick after the step.
static Decision ruleBankThenStepThenSpore(const Cell& me, const World&) {
  if (me.strength != Strength::Strongest)            return Action::idle();
  if (!me.canAffordSpore())                          return Action::idle();   // saving
  if (me.neighbour(Dir::W).isEdge())                 return Action::move(Dir::E);
  if (me.canSpore(Dir::E))                           return Action::spore(Dir::E);
  return Action::idle();
}

// Two cells aimed at one tile from different sides.
static Decision ruleConvergeOnOneTile(const Cell& me, const World&) {
  if (me.strength == Strength::Weak) return Action::move(Dir::E);
  return Action::move(Dir::N);
}

// ---- tests -----------------------------------------------------------------

// Grow claims the neighbour and the new cell starts at Weak, not at the
// parent's strength.
void test_grow_claims_neighbour_at_weak(void) {
  static const RuleFn rules[] = { ruleGrowNorth };
  Scenario s;
  s.clear();
  s.put(4, 4, 1, 3);
  s.start(rules, 1);
  s.load(1);
  s.tick();
  s.read();

  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(4, 3));
  TEST_ASSERT_EQUAL_UINT8(1, s.strAt(4, 3));    // new cells are Weak
  TEST_ASSERT_EQUAL_UINT8(3, s.strAt(4, 4));    // parent unchanged
}

// Energy belongs to the cell, not the team: one cell asking for something it
// cannot afford has no effect whatsoever on what any other cell can do. (This
// is the case v1 got wrong -- the unaffordable Spore used to starve the Grow
// behind it, because both were drawing on one shared purse.)
void test_a_cell_spends_only_its_own_purse(void) {
  static const RuleFn rules[] = { ruleSporeOrGrow };
  Scenario s;
  s.clear();
  s.put(1, 1, 1, 4);      // asks for a 6-cost Spore holding 2; earlier in scan order
  s.put(5, 5, 1, 1);      // asks for a 2-cost Grow holding 2
  s.start(rules, 1);
  s.load(1);
  s.tick();
  s.read();

  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(5, 4));            // the Grow went through
  TEST_ASSERT_EQUAL_UINT8(OWNER_EMPTY, s.ownerAt(1, 4));  // the Spore was unaffordable
  TEST_ASSERT_EQUAL_INT(3, s.cellCount(1));
}

// A share smaller than one energy is kept, not rounded away: eight cells split
// an income of 4, so each earns half a tick and can afford its first 1-cost
// action on the second tick, not the first and not never.
void test_fractional_share_accumulates(void) {
  static const RuleFn rules[] = { ruleFortify };
  Scenario s;
  s.clear();
  for (int x = 0; x < 8; x++) s.put(x, 0, 1, 1);
  s.start(rules, 1);
  s.load(1);

  s.tick();
  s.read();
  for (int x = 0; x < 8; x++)
    TEST_ASSERT_EQUAL_UINT8(1, s.strAt(x, 0));   // 0.75 banked: cannot afford 1 yet

  s.tick();
  s.read();
  for (int x = 0; x < 8; x++)
    TEST_ASSERT_EQUAL_UINT8(2, s.strAt(x, 0));   // 1.5 banked: fortified
}

// Damage is measured against the strength in the snapshot, so fortifying on
// the tick you take lethal damage does not save you -- and the tile it leaves
// is Neutral, not the attacker's.
void test_fortify_cannot_save_a_doomed_cell(void) {
  static const RuleFn rules[] = { ruleAttack, ruleFortify };
  Scenario s;
  s.clear();
  s.put(2, 2, 1, 2);      // attacker
  s.put(3, 2, 2, 1);      // defender: fortifies this tick, but dies anyway
  s.put(7, 7, 2, 1);      // keeps player 2 alive so the match does not end
  s.start(rules, 2);
  s.load(2);
  s.tick();
  s.read();

  TEST_ASSERT_EQUAL_UINT8(OWNER_NEUTRAL, s.ownerAt(3, 2));   // died despite fortifying
  TEST_ASSERT_EQUAL_UINT8(0, s.strAt(3, 2));
  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(2, 2));               // killing does not claim
  TEST_ASSERT_EQUAL_UINT8(2, s.ownerAt(7, 7));               // player 2's other cell survives
  TEST_ASSERT_EQUAL_UINT8(2, s.strAt(7, 7));                 // and its fortify did land
}

// Two viruses growing into one tile annihilate: nobody takes it, and both
// already paid for the attempt.
void test_contested_grow_annihilates(void) {
  static const RuleFn rules[] = { ruleGrowEast, ruleGrowWest };
  Scenario s;
  s.clear();
  s.put(2, 2, 1, 2);
  s.put(4, 2, 2, 2);
  s.start(rules, 2);
  s.load(2);
  s.tick();
  s.read();

  TEST_ASSERT_EQUAL_UINT8(OWNER_EMPTY, s.ownerAt(3, 2));   // contested: stays open
  TEST_ASSERT_EQUAL_INT(1, s.cellCount(1));
  TEST_ASSERT_EQUAL_INT(1, s.cellCount(2));
}

// Spore leaps whatever is in between and lands three tiles away -- but one cell
// has to save all six energy on its own. Two cells split an income of 4, so the
// sporer earns 2 a tick and leaps on the third.
void test_spore_banks_energy_then_leaps_a_blockade(void) {
  static const RuleFn rules[] = { ruleSporeEastWhenStrongest, ruleIdle };
  Scenario s;
  s.clear();
  s.put(0, 4, 1, 4);      // sporer
  s.put(7, 0, 1, 1);      // one idle cell, purely to halve the income
  s.put(1, 4, 2, 2);      // blockade it cannot grow through
  s.put(2, 4, 2, 2);
  s.start(rules, 2);
  s.load(2);
  s.requireIncome(4);

  s.tick(2);              // 4 banked: still short
  s.read();
  TEST_ASSERT_EQUAL_UINT8(OWNER_EMPTY, s.ownerAt(3, 4));

  s.tick();               // 6 banked: leaps both blockers
  s.read();
  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(3, 4));
  TEST_ASSERT_EQUAL_UINT8(1, s.strAt(3, 4));   // lands Weak
  TEST_ASSERT_EQUAL_UINT8(2, s.ownerAt(1, 4)); // leapt, not damaged
  TEST_ASSERT_EQUAL_UINT8(2, s.ownerAt(2, 4));
}

// A Move relocates the cell itself: the strength goes with it and the tile it
// left goes empty, not scorched. Nobody's cell count changes.
void test_move_carries_strength_and_vacates(void) {
  static const RuleFn rules[] = { ruleMoveEast };
  Scenario s;
  s.clear();
  s.put(4, 4, 1, 3);
  s.start(rules, 1);
  s.load(1);
  s.tick();
  s.read();

  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(5, 4));             // arrived
  TEST_ASSERT_EQUAL_UINT8(3, s.strAt(5, 4));               // with its strength intact
  TEST_ASSERT_EQUAL_UINT8(OWNER_EMPTY, s.ownerAt(4, 4));   // left open ground, not scorched
  TEST_ASSERT_EQUAL_INT(1, s.cellCount(1));                // moving is not growing
}

// The cell's banked energy travels with it. Four cells split an income of 4, so
// the sporer earns exactly 1 a tick: it banks six ticks, spends 1 stepping east
// off the rim on tick 6, and on tick 7 is back to 6 and leaps. Had the purse
// been left behind, tick 7 would find it holding 1 and going nowhere.
void test_move_carries_the_cells_purse(void) {
  static const RuleFn rules[] = { ruleBankThenStepThenSpore, ruleIdle };
  Scenario s;
  s.clear();
  s.put(0, 4, 1, 4);                              // the saver
  for (int x = 0; x < 3; x++) s.put(x, 0, 1, 1);  // idle filler: income / 4
  s.put(7, 7, 2, 1);
  s.start(rules, 2);
  s.load(2);
  s.requireIncome(4);

  s.tick(6);
  s.read();
  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(1, 4));             // stepped east
  TEST_ASSERT_EQUAL_UINT8(OWNER_EMPTY, s.ownerAt(0, 4));
  TEST_ASSERT_EQUAL_UINT8(OWNER_EMPTY, s.ownerAt(4, 4));   // has not spored yet

  s.tick();
  s.read();
  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(4, 4));             // spored the tick after moving
  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(1, 4));             // and is still standing where it landed
}

// A Move that loses the tile it aimed at leaves the mover exactly where it was.
// Getting this wrong deletes cells, which is the worst bug this rule can have.
void test_contested_move_leaves_the_mover_in_place(void) {
  static const RuleFn rules[] = { ruleMoveEast, ruleGrowWest };
  Scenario s;
  s.clear();
  s.put(2, 2, 1, 2);      // walking east into (3,2)
  s.put(4, 2, 2, 2);      // growing west into (3,2)
  s.start(rules, 2);
  s.load(2);
  s.tick();
  s.read();

  TEST_ASSERT_EQUAL_UINT8(OWNER_EMPTY, s.ownerAt(3, 2));   // contested: nobody takes it
  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(2, 2));             // and the mover did not vanish
  TEST_ASSERT_EQUAL_UINT8(2, s.strAt(2, 2));
  TEST_ASSERT_EQUAL_INT(1, s.cellCount(1));
  TEST_ASSERT_EQUAL_INT(1, s.cellCount(2));
}

// Attackers hit a cell where it stood. A mover that takes lethal damage on the
// way out dies at its old tile and never arrives, so the tile it was walking
// into stays open.
void test_a_killed_mover_never_arrives(void) {
  static const RuleFn rules[] = { ruleMoveEast, ruleAttack };
  Scenario s;
  s.clear();
  s.put(2, 2, 1, 1);      // walking east, and one hit from death
  s.put(1, 2, 2, 2);      // attacking it
  s.start(rules, 2);
  s.load(2);
  s.tick();
  s.read();

  TEST_ASSERT_EQUAL_UINT8(OWNER_NEUTRAL, s.ownerAt(2, 2));  // died where it stood
  TEST_ASSERT_EQUAL_UINT8(OWNER_EMPTY,   s.ownerAt(3, 2));  // never landed
  TEST_ASSERT_EQUAL_INT(0, s.cellCount(1));
}

// Everything is decided against the snapshot, so a cell cannot walk into a tile
// one of its own is vacating this same tick -- no conga lines, no swaps.
void test_no_chain_move(void) {
  static const RuleFn rules[] = { ruleMoveEast };
  Scenario s;
  s.clear();
  s.put(2, 2, 1, 1);      // aims at (3,2), which is occupied in the snapshot
  s.put(3, 2, 1, 2);      // aims at (4,2), which is open
  s.start(rules, 1);
  s.load(1);
  s.tick();
  s.read();

  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(4, 2));             // the front cell moved
  TEST_ASSERT_EQUAL_UINT8(2, s.strAt(4, 2));
  TEST_ASSERT_EQUAL_UINT8(OWNER_EMPTY, s.ownerAt(3, 2));   // and did not tow the one behind
  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(2, 2));
  TEST_ASSERT_EQUAL_UINT8(1, s.strAt(2, 2));
  TEST_ASSERT_EQUAL_INT(2, s.cellCount(1));
}

// Two of your own cells aimed at one tile do not annihilate each other -- the
// first in scan order takes it and the second simply stays put.
void test_two_of_your_cells_moving_into_one_tile(void) {
  static const RuleFn rules[] = { ruleConvergeOnOneTile };
  Scenario s;
  s.clear();
  s.put(2, 2, 1, 1);      // Weak: walks east into (3,2). Earlier in scan order.
  s.put(3, 3, 1, 2);      // Normal: walks north into (3,2)
  s.start(rules, 1);
  s.load(1);
  s.tick();
  s.read();

  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(3, 2));
  TEST_ASSERT_EQUAL_UINT8(1, s.strAt(3, 2));               // the Weak one got there
  TEST_ASSERT_EQUAL_UINT8(OWNER_EMPTY, s.ownerAt(2, 2));
  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(3, 3));             // the other one never left
  TEST_ASSERT_EQUAL_UINT8(2, s.strAt(3, 3));
  TEST_ASSERT_EQUAL_INT(2, s.cellCount(1));
}

// Movement counts as progress. A cell walking across the board changes nobody's
// cell count, so territory alone would call this stalled after 24 ticks -- the
// centroid is what keeps it alive. Once it hits the east rim and stops, the
// stall detector does fire, one full window later.
void test_movement_defers_the_stalemate(void) {
  static const RuleFn rules[] = { ruleWalkEast, ruleIdle };
  Scenario s;
  s.clear();
  s.put(0, 3, 1, 1);      // walks east, reaching the rim on tick 7
  s.put(0, 7, 2, 1);      // sits still
  s.start(rules, 2);
  s.load(2);

  s.tick(30);             // window ago it was still one tile further west
  TEST_ASSERT_FALSE_MESSAGE(s.isOver(), "a moving virus was called stalled");

  s.tick();               // a full window with nothing moving at all
  TEST_ASSERT_TRUE_MESSAGE(s.isOver(), "a frozen board was not called stalled");
}

// The whole point of packing: a full 16x16 board has to cross the wire in one
// ESP-NOW frame. One byte per cell would be 261 -- over the payload cap -- so
// this is the assertion that guards Virus multiplayer on the bigger panel.
void test_16x16_snapshot_fits_one_frame(void) {
  static const RuleFn rules[] = { ruleGrowNorth, ruleGrowEast };
  Virus  g;
  uint8_t idx[2] = { 0, 1 };
  g.setRoster(rules, idx, 2);
  g.begin(16, 16, 0, 2, 0xC0FFEE, nullptr);

  uint8_t buf[512];
  size_t  len = g.serializeState(buf, sizeof(buf));

  TEST_ASSERT_EQUAL_size_t(199, len);                      // 7 header + 192 packed
  TEST_ASSERT_TRUE_MESSAGE(len <= NET_STATE_MAX, "16x16 snapshot exceeds NET_STATE_MAX");
  TEST_ASSERT_TRUE_MESSAGE(len + sizeof(NetHeader) <= NET_MAX_PAYLOAD,
                           "16x16 frame exceeds the ESP-NOW payload cap");
}

// A client rebuilds exactly the host's board: every owner and strength value
// survives the round trip, including scorched ground and Strongest cells.
void test_state_round_trips_every_cell_value(void) {
  static const RuleFn rules[] = { ruleIdle };
  Scenario s;
  s.clear();
  // Sweep the whole encodable range across the board: each owner code paired
  // with each strength, so a packing slip in any bit position shows up.
  for (int c = 0; c < N; c++) {
    uint8_t owner = (uint8_t)(c % 7);                       // 0..6: empty, 1-5, neutral
    uint8_t str   = (owner == 0 || owner == OWNER_NEUTRAL) ? 0 : (uint8_t)(1 + (c % 4));
    s.owner[c] = owner;
    s.str[c]   = str;
  }
  uint8_t wantOwner[N], wantStr[N];
  memcpy(wantOwner, s.owner, N);
  memcpy(wantStr,   s.str,   N);

  s.start(rules, 1);
  s.load(1);
  s.read();                                                 // no tick: pure round trip

  TEST_ASSERT_EQUAL_UINT8_ARRAY(wantOwner, s.owner, N);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(wantStr,   s.str,   N);
}

// ---- lockstep multiplayer ---------------------------------------------------
// Each player's rule is compiled into their own device, so what crosses the
// wire is the decision that rule reached: five bits per owned cell, in scan
// order, no cell indices. These pin the two ways that can silently go wrong --
// the two ends disagreeing about the order, and a list surviving onto a board
// it was not computed against.

// Every action kind and direction, keyed off position so the packed list has a
// different value in every entry. The N/E/S/W cycle also guarantees the 5-bit
// fields straddle byte boundaries in every alignment.
static Decision ruleEveryAction(const Cell& me, const World&) {
  switch (me.strength) {
    case Strength::Weak:      return Action::grow(Dir::N);
    case Strength::Normal:    return Action::attack(Dir::E);
    case Strength::Strong:    return Action::move(Dir::S);
    case Strength::Strongest: return Action::spore(Dir::W);
    default:                  return Action::fortify();
  }
}

// Hand-build one entry of a decision list. A deliberate second implementation
// of the packing -- only ever used to forge lists the referee should REFUSE, so
// it never stands in for the real codec. That is exercised end to end by
// test_networked_decisions_match_a_local_match, which runs the actual packer on
// one Virus and the actual unpacker on another.
static void putTestDecision(uint8_t* buf, int index, const Action& a) {
  const uint8_t v = (uint8_t)(((uint8_t)a.kind & 0x07) | (((uint8_t)a.dir & 0x03) << 3));
  const int bit = index * 5, byte = bit >> 3, off = bit & 7;
  buf[byte] |= (uint8_t)(v << off);
  if (off > 3) buf[byte + 1] |= (uint8_t)(v >> (8 - off));
}

// Drive one Virus as a client of another: adopt the host's board and energy
// report, run the local rule, hand the packed decisions back to the host.
// This is the whole networked path, minus the radio.
static void relayDecisions(Virus& host, Virus& client, uint8_t pid) {
  uint8_t sb[512];
  size_t  sn = host.serializeState(sb, sizeof(sb));
  client.applyState(sb, sn);

  uint8_t pb[NET_PRIV_MAX];
  size_t  pn = host.serializePrivate(pid, pb, sizeof(pb));
  client.applyPrivate(pb, pn);

  uint8_t db[NET_INPUT_MAX];
  size_t  dn = client.decideLocal(db, sizeof(db), false);
  host.applyInput(pid, db, dn);
}

// The test the whole design rests on: a match where player 1's decisions arrive
// over the wire has to be bit-identical to the same match played locally. If
// the two ends ever disagree about which cell is entry 7, this diverges.
void test_networked_decisions_match_a_local_match(void) {
  static const RuleFn localRules[]  = { ruleEveryAction, ruleEveryAction };
  static const RuleFn hostRules[]   = { ruleEveryAction, nullptr };   // p1 is remote
  static const RuleFn clientRules[] = { nullptr, ruleEveryAction };   // p1 is us

  Scenario ref, host, client;
  for (Scenario* s : { &ref, &host, &client }) {
    s->clear();
    s->put(2, 2, 1, 1); s->put(3, 5, 1, 3); s->put(6, 1, 1, 2);
    s->put(5, 5, 2, 1); s->put(1, 6, 2, 4); s->put(6, 6, 2, 2);
  }
  static const uint8_t idx[2] = { 0, 1 };
  ref.start(localRules, 2);      ref.load(2);
  host.start(hostRules, 2);      host.load(2);
  client.game.setRoster(clientRules, idx, 2);
  client.game.begin(W, H, 1, 2, 0xC0FFEE, nullptr);   // myId = 1

  for (int t = 0; t < 12; t++) {
    relayDecisions(host.game, client.game, 1);
    ref.tick();
    host.tick();

    ref.read();
    host.read();
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(ref.owner, host.owner, N,
                                          "networked board diverged from local");
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(ref.str, host.str, N,
                                          "networked strengths diverged from local");
  }
}

// A player whose decisions never arrive idles: it keeps its ground and its
// strength, and everyone else plays on untouched. This is the disconnect
// behaviour, and it is the same path a dropped or mistagged list takes.
void test_a_player_with_no_decisions_idles(void) {
  static const RuleFn rules[] = { ruleGrowEast, nullptr };
  Scenario s;
  s.clear();
  s.put(1, 1, 1, 2);
  s.put(6, 6, 2, 3);      // remote, and nothing ever arrives for it
  s.start(rules, 2);
  s.load(2);
  s.tick(3);
  s.read();

  TEST_ASSERT_EQUAL_UINT8(2, s.ownerAt(6, 6));   // still there
  TEST_ASSERT_EQUAL_UINT8(3, s.strAt(6, 6));     // and unchanged: it did nothing
  TEST_ASSERT_EQUAL_INT(1, s.cellCount(2));
  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(2, 1));   // the live player carried on
}

// A list sized for a different cell count was computed against a different
// board, so its entries line up with the wrong cells. Length alone catches it,
// independently of the tick tag the runner checks, and the player idles.
void test_a_misaligned_decision_list_is_rejected(void) {
  static const RuleFn rules[] = { ruleIdle, nullptr };
  Scenario s;
  s.clear();
  s.put(0, 0, 1, 1);
  s.put(4, 4, 2, 2); s.put(5, 4, 2, 2); s.put(6, 4, 2, 2);   // remote owns 3
  s.start(rules, 2);
  s.load(2);

  // A well-formed list for five cells: right shape, wrong board.
  uint8_t bogus[NET_INPUT_MAX] = { 0 };
  for (int i = 0; i < 5; i++) putTestDecision(bogus, i, Action::grow(Dir::E));
  s.game.applyInput(1, bogus, Virus::decisionBytes(5));

  s.tick();
  s.read();
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, s.cellCount(2), "a misaligned list was acted on");
}

// A list is good for exactly the tick it was computed against, so the referee
// must consume it rather than hold it. Feed one list, tick twice: the second
// tick has to find nothing.
void test_a_decision_list_is_consumed_by_one_tick(void) {
  static const RuleFn rules[] = { ruleIdle, nullptr };
  Scenario s;
  s.clear();
  s.put(0, 0, 1, 1);
  s.put(4, 4, 2, 2);
  s.start(rules, 2);
  s.load(2);

  uint8_t db[NET_INPUT_MAX] = { 0 };
  putTestDecision(db, 0, Action::grow(Dir::E));
  s.game.applyInput(1, db, Virus::decisionBytes(1));

  s.tick();
  s.read();
  TEST_ASSERT_EQUAL_UINT8(2, s.ownerAt(5, 4));   // the one grow landed
  TEST_ASSERT_EQUAL_INT(2, s.cellCount(2));

  s.tick();                                       // nothing new supplied
  s.read();
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, s.cellCount(2), "a stale list was replayed");
}

// The energy report is what lets a client answer canAfford() at all, so it has
// to survive the round trip exactly -- including both ends of the 4-bit range.
void test_energy_report_round_trips(void) {
  static const RuleFn rules[] = { ruleIdle, nullptr };
  Scenario s;
  s.clear();
  for (int x = 0; x < 4; x++) s.put(x, 0, 1, 1);
  s.put(7, 7, 2, 1);
  s.start(rules, 2);
  s.load(2);
  s.requireIncome(4);

  uint8_t pb[NET_PRIV_MAX];
  size_t  pn = s.game.serializePrivate(0, pb, sizeof(pb));
  TEST_ASSERT_EQUAL_size_t(Virus::energyBytes(4), pn);

  // Four cells splitting an income of 4 is one whole unit each, credited before
  // the tick -- so that is what the report has to say.
  for (int i = 0; i < 4; i++)
    TEST_ASSERT_EQUAL_UINT8(1, (pb[i >> 1] >> ((i & 1) ? 4 : 0)) & 0x0F);
}

// The full-board worst case is not hypothetical -- it is what the board looks
// like just before a match ends. Both lists have to fit their frames there.
void test_worst_case_lists_fit_their_frames(void) {
  const size_t d = Virus::decisionBytes(VIRUS_MAX_CELLS);
  const size_t e = Virus::energyBytes(VIRUS_MAX_CELLS);
  TEST_ASSERT_EQUAL_size_t(160, d);
  TEST_ASSERT_TRUE_MESSAGE(d <= NET_INPUT_MAX, "decision list exceeds NET_INPUT_MAX");
  TEST_ASSERT_TRUE_MESSAGE(e <= NET_PRIV_MAX,  "energy report exceeds NET_PRIV_MAX");
  TEST_ASSERT_TRUE_MESSAGE(d + sizeof(NetHeader) <= NET_MAX_PAYLOAD,
                           "decision frame exceeds the ESP-NOW payload cap");
}

// A networked device brings one rule and does not know which player it will be
// until the lobby says. Placing it on the wrong slot would run our virus as
// somebody else's -- which reads as a mysteriously badly-played opponent rather
// than as a bug, so it is worth pinning. The decision list is sized by the
// owning player's cell count, so the length alone says which slot was used.
void test_local_rule_lands_on_the_assigned_slot(void) {
  Scenario s;
  s.clear();
  s.put(0, 0, 1, 1);                                   // player 1: one cell
  s.put(2, 0, 2, 1); s.put(3, 0, 2, 1);                // player 2: two
  s.put(5, 0, 3, 1); s.put(6, 0, 3, 1); s.put(7, 0, 3, 1);   // player 3: three

  s.game.setLocalRule(ruleGrowEast, 2);
  s.game.begin(W, H, 2, 3, 0xC0FFEE, nullptr);         // the lobby made us player 3
  s.load(3);

  uint8_t db[NET_INPUT_MAX];
  const size_t dn = s.game.decideLocal(db, sizeof(db), false);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(Virus::decisionBytes(3), dn,
                                   "decided for the wrong player's cells");
}

// setRoster is the single-device path and has to win: every slot local, nothing
// waiting on the wire. If a stale local rule survived, the other players would
// silently idle through the whole match.
void test_set_roster_clears_a_networked_local_rule(void) {
  static const RuleFn rules[] = { ruleGrowEast, ruleGrowWest };
  Scenario s;
  s.clear();
  s.put(1, 4, 1, 1);
  s.put(6, 4, 2, 1);

  s.game.setLocalRule(ruleGrowEast, 0);   // as if we had just played networked
  s.start(rules, 2);                      // then went back to a local match
  s.load(2);
  s.tick();
  s.read();

  TEST_ASSERT_EQUAL_UINT8(1, s.ownerAt(2, 4));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, s.ownerAt(5, 4), "player 2 idled: local rule leaked");
}

// The territory bar reads cellCount(), and the host fills those in
// evaluateEnd() -- which a client never runs, because a client never ticks.
// Adopting a board has to recount, or a networked player watches an empty bar
// for the whole match.
void test_adopting_a_board_recounts_territory(void) {
  static const RuleFn rules[] = { ruleIdle, ruleIdle };
  Scenario s;
  s.clear();
  s.put(1, 1, 1, 1); s.put(2, 1, 1, 1); s.put(3, 1, 1, 1);
  s.put(6, 6, 2, 1); s.put(7, 6, 2, 1);
  s.start(rules, 2);
  s.load(2);                       // applyState, exactly as a client does

  TEST_ASSERT_EQUAL_UINT16(3, s.game.cellCount(0));
  TEST_ASSERT_EQUAL_UINT16(2, s.game.cellCount(1));
}

// ---- fixed openings --------------------------------------------------------
// Unlike every test above, these read the board straight after begin() and
// never call load() -- the seeds ARE what is under test, and load() overwrites
// the whole board.

// Where each virus was seeded for one (opening, roster size). Indexed by
// player - 1; -1 means that virus was never placed.
struct Opening {
  int x[4], y[4];
  int count;          // owned cells on the board in total
};

static Opening readOpening(uint8_t opening, uint8_t numPlayers) {
  static const RuleFn rules[] = { ruleIdle, ruleIdle, ruleIdle, ruleIdle };
  Scenario s;
  s.game.setOpening(opening);
  s.start(rules, numPlayers);
  s.read();

  Opening o;
  o.count = 0;
  for (int i = 0; i < 4; i++) { o.x[i] = -1; o.y[i] = -1; }
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const uint8_t p = s.ownerAt(x, y);
      if (p == OWNER_EMPTY || p == OWNER_NEUTRAL) continue;
      o.count++;
      if (p >= 1 && p <= 4) { o.x[p - 1] = x; o.y[p - 1] = y; }
    }
  return o;
}

// One seed per virus, nobody sharing a tile, at every roster size.
void test_every_opening_seeds_one_cell_per_virus(void) {
  for (uint8_t op = 0; op < VIRUS_OPENINGS; op++) {
    for (uint8_t n = 1; n <= 4; n++) {
      const Opening o = readOpening(op, n);
      TEST_ASSERT_EQUAL_INT_MESSAGE((int)n, o.count,
        "an opening must seed exactly one cell per virus");
      for (int p = 0; p < (int)n; p++) {
        TEST_ASSERT_TRUE_MESSAGE(o.x[p] >= 0, "a virus was left off the board");
        for (int q = p + 1; q < (int)n; q++)
          TEST_ASSERT_FALSE_MESSAGE(o.x[p] == o.x[q] && o.y[p] == o.y[q],
            "two viruses were seeded onto the same tile");
      }
    }
  }
}

// Inset by one on every side, so each virus opens with the same four legal
// neighbours. A seed in a literal corner would have two, which is a handicap
// rather than a difference in spacing.
void test_openings_keep_every_seed_off_the_wall(void) {
  for (uint8_t op = 0; op < VIRUS_OPENINGS; op++) {
    for (uint8_t n = 1; n <= 4; n++) {
      const Opening o = readOpening(op, n);
      for (int p = 0; p < (int)n; p++) {
        TEST_ASSERT_TRUE_MESSAGE(o.x[p] >= 1 && o.x[p] <= W - 2, "seed on a side wall");
        TEST_ASSERT_TRUE_MESSAGE(o.y[p] >= 1 && o.y[p] <= H - 2, "seed on the top or bottom wall");
      }
    }
  }
}

// A short roster takes the spots furthest apart, so two viruses never open the
// match already touching.
void test_two_viruses_never_start_next_to_each_other(void) {
  for (uint8_t op = 0; op < VIRUS_OPENINGS; op++) {
    const Opening o = readOpening(op, 2);
    const int dx = o.x[0] - o.x[1], dy = o.y[0] - o.y[1];
    TEST_ASSERT_TRUE_MESSAGE(dx * dx + dy * dy > 1,
      "a two-virus roster must not start orthogonally adjacent");
  }
}

// Slot assignment rotates with the opening, so one virus never plays the same
// spot twice in a series. Without this, part of a virus's score is just where
// it happened to sit -- the find* helpers stop at the first hit and drift north
// and east, so the spots are not interchangeable.
void test_a_series_moves_every_virus_around_the_board(void) {
  int xs[VIRUS_OPENINGS], ys[VIRUS_OPENINGS];
  for (uint8_t op = 0; op < VIRUS_OPENINGS; op++) {
    const Opening o = readOpening(op, 4);
    xs[op] = o.x[0]; ys[op] = o.y[0];          // virus A
  }
  for (int i = 0; i < VIRUS_OPENINGS; i++)
    for (int j = i + 1; j < VIRUS_OPENINGS; j++)
      TEST_ASSERT_FALSE_MESSAGE(xs[i] == xs[j] && ys[i] == ys[j],
        "virus A started twice in the same spot inside one series");
}

// setOpening() takes a round number and wraps, so a caller never has to know
// how many openings exist.
void test_opening_wraps_past_the_last_one(void) {
  const Opening first   = readOpening(0, 4);
  const Opening wrapped = readOpening(VIRUS_OPENINGS, 4);
  for (int p = 0; p < 4; p++) {
    TEST_ASSERT_EQUAL_INT(first.x[p], wrapped.x[p]);
    TEST_ASSERT_EQUAL_INT(first.y[p], wrapped.y[p]);
  }
}

// The one opening pinned to exact tiles, so the diagonal-first ordering that
// every roster size depends on cannot drift unnoticed: TL, BR, TR, BL.
void test_the_corner_opening_puts_one_virus_in_each_corner(void) {
  const Opening o = readOpening(0, 4);
  TEST_ASSERT_EQUAL_INT(1,     o.x[0]);  TEST_ASSERT_EQUAL_INT(1,     o.y[0]);
  TEST_ASSERT_EQUAL_INT(W - 2, o.x[1]);  TEST_ASSERT_EQUAL_INT(H - 2, o.y[1]);
  TEST_ASSERT_EQUAL_INT(W - 2, o.x[2]);  TEST_ASSERT_EQUAL_INT(1,     o.y[2]);
  TEST_ASSERT_EQUAL_INT(1,     o.x[3]);  TEST_ASSERT_EQUAL_INT(H - 2, o.y[3]);
}

// A bare grower that rotates its scan with the tick -- the behaviour under
// test. Deliberately NOT decideA: the starters exist to be rewritten, and a
// fairness test must not break the first time somebody improves one.
static Decision ruleGrowRotating(const Cell& me, const World& world) {
  Dir d = Dir::N;
  if (me.findOpen(d, (Dir)(world.tick & 3)) && me.canGrow(d)) return Action::grow(d);
  return Action::idle();
}

// The same rule with its scan pinned north, which is what the drift looks like.
static Decision ruleGrowFixed(const Cell& me, const World&) {
  Dir d = Dir::N;
  if (me.findOpen(d) && me.canGrow(d)) return Action::grow(d);
  return Action::idle();
}

static int cornerSpread(RuleFn r, int ticks) {
  const RuleFn rules[] = { r, r, r, r };
  Scenario s;
  s.game.setOpening(0);      // one virus per corner
  s.start(rules, 4);
  s.tick(ticks);
  s.read();
  int lo = 9999, hi = 0;
  for (uint8_t p = 1; p <= 4; p++) {
    const int c = s.cellCount(p);
    if (c < lo) lo = c;
    if (c > hi) hi = c;
  }
  return hi - lo;
}

// findOpen begins where it is told and wraps, which is the whole mechanism --
// the same open field yields a different direction as the tick advances.
void test_find_open_starts_where_it_is_told(void) {
  Cell me;
  memset(&me, 0, sizeof(me));
  me.strength = Strength::Normal;
  me.energy   = 8;
  for (int i = 0; i < 4; i++) me.n[i].occupant = Occupant::Empty;

  Dir d = Dir::N;
  for (int s = 0; s < 4; s++) {          // wide open: it takes the one asked for
    TEST_ASSERT_TRUE(me.findOpen(d, (Dir)s));
    TEST_ASSERT_EQUAL_INT_MESSAGE(s, (int)d, "findOpen ignored its start direction");
  }

  for (int i = 1; i < 4; i++) me.n[i].occupant = Occupant::Wall;
  for (int s = 0; s < 4; s++) {          // only North open: every start wraps to it
    TEST_ASSERT_TRUE(me.findOpen(d, (Dir)s));
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)Dir::N, (int)d, "findOpen failed to wrap");
  }

  // Omitting the argument still means the original N,E,S,W order, so every rule
  // written before this existed behaves exactly as it did.
  for (int i = 0; i < 4; i++) me.n[i].occupant = Occupant::Empty;
  TEST_ASSERT_TRUE(me.findOpen(d));
  TEST_ASSERT_EQUAL_INT((int)Dir::N, (int)d);
}

// Four identical viruses, one per corner of a square board. The corners are
// equivalent positions, so a fair rule should finish them near-level -- and a
// rule that always looks north should not, because a fixed preference breaks
// the board's symmetry (north-west is against the rim after one step, south-east
// has the whole board ahead of it).
//
// Measured when this was written: fixed scan 13/17/15/19, rotating 15/16/16/17.
// The comparison is the real assertion; the absolute bound is a tripwire.
void test_rotating_the_scan_levels_out_the_corners(void) {
  const int rotating = cornerSpread(ruleGrowRotating, 30);
  const int fixed    = cornerSpread(ruleGrowFixed,    30);
  TEST_ASSERT_LESS_THAN_INT_MESSAGE(fixed, rotating,
    "a rotating scan must even the corners out more than a fixed N,E,S,W one");
  TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(3, rotating,
    "corner-to-corner spread has grown -- the scan rotation may have regressed");
}

// The series length is chosen before a match and clamped, because it arrives
// from two places that can both get it wrong: a setup screen, and a lobby frame
// from another device. A zero would end the series before its first result
// screen; anything past VIRUS_OPENINGS would ask for a layout that does not
// exist.
void test_series_length_is_clamped_to_something_playable(void) {
  Virus g;

  TEST_ASSERT_EQUAL_UINT8_MESSAGE(VIRUS_ROUNDS_DEFAULT, g.seriesRounds(),
    "a Virus that was never told should default to the short series");

  for (uint8_t r = VIRUS_ROUNDS_MIN; r <= VIRUS_SERIES_ROUNDS; r++) {
    g.setSeriesRounds(r);
    TEST_ASSERT_EQUAL_UINT8(r, g.seriesRounds());
  }

  g.setSeriesRounds(0);                       // an old or silent lobby frame
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(VIRUS_ROUNDS_MIN, g.seriesRounds(),
    "0 rounds would end the series before a single match was played");

  g.setSeriesRounds(200);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(VIRUS_SERIES_ROUNDS, g.seriesRounds(),
    "a series must not outrun the openings there are to play");

  // seriesRounds() is what the shell counts to, and maxSeriesRounds() is what
  // the lobby offers -- non-zero is what tells it to draw a picker at all.
  TEST_ASSERT_EQUAL_UINT8(VIRUS_SERIES_ROUNDS, g.maxSeriesRounds());
}

// Every length a player can pick has an opening to play, and each round of a
// series draws a different one.
void test_every_round_of_a_series_has_its_own_opening(void) {
  for (uint8_t rounds = VIRUS_ROUNDS_MIN; rounds <= VIRUS_SERIES_ROUNDS; rounds++) {
    int xs[VIRUS_SERIES_ROUNDS], ys[VIRUS_SERIES_ROUNDS];
    for (uint8_t round = 0; round < rounds; round++) {
      const Opening o = readOpening(round, 4);
      TEST_ASSERT_EQUAL_INT_MESSAGE(4, o.count, "a round seeded the wrong number of viruses");
      xs[round] = o.x[0]; ys[round] = o.y[0];
    }
    for (int i = 0; i < (int)rounds; i++)
      for (int j = i + 1; j < (int)rounds; j++)
        TEST_ASSERT_FALSE_MESSAGE(xs[i] == xs[j] && ys[i] == ys[j],
          "two rounds of the same series opened on the same board");
  }
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_grow_claims_neighbour_at_weak);
  RUN_TEST(test_a_cell_spends_only_its_own_purse);
  RUN_TEST(test_fractional_share_accumulates);
  RUN_TEST(test_fortify_cannot_save_a_doomed_cell);
  RUN_TEST(test_contested_grow_annihilates);
  RUN_TEST(test_spore_banks_energy_then_leaps_a_blockade);
  RUN_TEST(test_move_carries_strength_and_vacates);
  RUN_TEST(test_move_carries_the_cells_purse);
  RUN_TEST(test_contested_move_leaves_the_mover_in_place);
  RUN_TEST(test_a_killed_mover_never_arrives);
  RUN_TEST(test_no_chain_move);
  RUN_TEST(test_two_of_your_cells_moving_into_one_tile);
  RUN_TEST(test_movement_defers_the_stalemate);
  RUN_TEST(test_networked_decisions_match_a_local_match);
  RUN_TEST(test_a_player_with_no_decisions_idles);
  RUN_TEST(test_a_misaligned_decision_list_is_rejected);
  RUN_TEST(test_a_decision_list_is_consumed_by_one_tick);
  RUN_TEST(test_energy_report_round_trips);
  RUN_TEST(test_worst_case_lists_fit_their_frames);
  RUN_TEST(test_local_rule_lands_on_the_assigned_slot);
  RUN_TEST(test_set_roster_clears_a_networked_local_rule);
  RUN_TEST(test_adopting_a_board_recounts_territory);
  RUN_TEST(test_16x16_snapshot_fits_one_frame);
  RUN_TEST(test_state_round_trips_every_cell_value);
  RUN_TEST(test_every_opening_seeds_one_cell_per_virus);
  RUN_TEST(test_openings_keep_every_seed_off_the_wall);
  RUN_TEST(test_two_viruses_never_start_next_to_each_other);
  RUN_TEST(test_a_series_moves_every_virus_around_the_board);
  RUN_TEST(test_opening_wraps_past_the_last_one);
  RUN_TEST(test_the_corner_opening_puts_one_virus_in_each_corner);
  RUN_TEST(test_find_open_starts_where_it_is_told);
  RUN_TEST(test_rotating_the_scan_levels_out_the_corners);
  RUN_TEST(test_series_length_is_clamped_to_something_playable);
  RUN_TEST(test_every_round_of_a_series_has_its_own_opening);
  return UNITY_END();
}
