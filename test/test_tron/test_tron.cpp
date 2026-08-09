// ============================================================================
//  Native tests for the Tron simulation (src/games/tron/tron.cpp).
//
//  Tron's rules are all timing and bookkeeping -- how often a head steps, how
//  long a trail stays behind it, and who dies when two heads want the same
//  cell. None of that is visible without two units and a stopwatch, which is
//  exactly why it is worth pinning here.
//
//  Run with:  pio test -e native
//
//  State is read back through serializeState(), the same view a client gets:
//  [phase, winner, numPlayers, {hx, hy, dir|alive}*N, nibble-packed owner map].
// ============================================================================
#include <unity.h>
#include <string.h>
#include "games/tron/tron.h"

// Mirrors of the tuning constants in tron.cpp, at 8x8. They are DERIVED from the
// arena now rather than fixed -- a 16x16 board gets twice the speed and a longer
// starting trail -- but the derivations were built to reproduce these exact
// numbers at 8x8, so every value below is unchanged from rev 1. If someone
// retunes the feel, these fail and say so rather than silently drifting.
static const int STEP_TICKS       = 20;   // stepTicksFor(8, 8, false)
static const int BOOST_STEP_TICKS = 10;   // stepTicksFor(8, 8, true)
static const int START_LEN        = 3;    // startLenFor(8, 8)
static const int GROW_TICKS       = 150;  // growTicksFor(8, 8)

// The same four at 16x16. Twice the speed, nearly three times the trail, twice
// the growth rate -- four times the ground has to play in a comparable stretch
// of time, and none of that happens if the numbers stay put.
static const int STEP_TICKS_16 = 10;      // stepTicksFor(16, 16, false)
static const int START_LEN_16  = 8;       // startLenFor(16, 16)
static const int GROW_TICKS_16 = 75;      // growTicksFor(16, 16)

static const uint8_t DIR_R = 0, DIR_D = 1, DIR_L = 2, DIR_U = 3;

// ---- reading the sim --------------------------------------------------------

struct View {
  uint8_t phase, winner, numPlayers;
  int     hx[NET_MAX_PLAYERS], hy[NET_MAX_PLAYERS];
  uint8_t dir[NET_MAX_PLAYERS];
  bool    alive[NET_MAX_PLAYERS];
  uint8_t owner[TRON_MAX_CELLS];
  int     cells;

  void read(Tron& g, int w, int h) {
    uint8_t buf[512];
    size_t  len = g.serializeState(buf, sizeof(buf));
    TEST_ASSERT_TRUE_MESSAGE(len > 0, "serializeState did not fit the buffer");
    size_t o = 0;
    phase = buf[o++]; winner = buf[o++]; numPlayers = buf[o++];
    for (uint8_t i = 0; i < numPlayers; i++) {
      hx[i] = buf[o++];
      hy[i] = buf[o++];
      uint8_t d = buf[o++];
      dir[i]   = d & 3;
      alive[i] = (d & 0x80) != 0;
    }
    cells = w * h;
    memset(owner, 0, sizeof(owner));
    for (int c = 0; c < cells; c += 2) {
      uint8_t b = buf[o++];
      owner[c] = b & 0x0F;
      if (c + 1 < cells) owner[c + 1] = (b >> 4) & 0x0F;
    }
  }

  // How many cells the given player's trail currently occupies.
  int trailCells(uint8_t playerId) const {
    int n = 0;
    for (int c = 0; c < cells; c++) if (owner[c] == playerId + 1) n++;
    return n;
  }
};

static void tick(Tron& g, int n) { for (int i = 0; i < n; i++) g.hostTick(); }

// Hold a heading/boost input for a player, the same two bytes a human's stick
// and A button would produce. Byte 0 is an ABSOLUTE heading (or TRON_NO_TURN),
// which is the rev 2 change: rev 1 sent a relative -1/0/+1 turn here.
static void hold(Tron& g, uint8_t pid, uint8_t heading, bool boost) {
  uint8_t in[2] = { heading, (uint8_t)(boost ? 1 : 0) };
  g.applyInput(pid, in, 2);
}

// Place heads explicitly. begin() spawns at fixed positions, which is fine for
// most tests; this is for the ones that need a specific geometry. Kept to a
// couple of steps afterwards, since it seeds the owner map without rebuilding
// each player's trail ring.
static void placeHeads(Tron& g, int w, int h, uint8_t numPlayers,
                       const int* hx, const int* hy, const uint8_t* dir) {
  uint8_t buf[512];
  size_t  o = 0;
  buf[o++] = 0;      // phase: running
  buf[o++] = 0xFF;   // winner: none
  buf[o++] = numPlayers;
  for (uint8_t i = 0; i < numPlayers; i++) {
    buf[o++] = (uint8_t)hx[i];
    buf[o++] = (uint8_t)hy[i];
    buf[o++] = (uint8_t)(dir[i] | 0x80);   // alive
  }
  uint8_t owner[TRON_MAX_CELLS] = { 0 };
  for (uint8_t i = 0; i < numPlayers; i++) owner[hy[i] * w + hx[i]] = i + 1;
  int cells = w * h;
  for (int c = 0; c < cells; c += 2) {
    uint8_t lo = owner[c] & 0x0F;
    uint8_t hi = (c + 1 < cells) ? (owner[c + 1] & 0x0F) : 0;
    buf[o++] = (uint8_t)(lo | (hi << 4));
  }
  g.applyState(buf, o);
}

// ---- tests ------------------------------------------------------------------

// A head steps once every BASE_STEP_TICKS, not once per tick. The sim ticks far
// faster than players move; that gap is what makes the game readable.
void test_head_steps_once_per_step_interval(void) {
  Tron g;
  g.begin(8, 8, 0, 1, 1234, nullptr);
  View v;

  v.read(g, 8, 8);
  const int x0 = v.hx[0], y0 = v.hy[0];
  TEST_ASSERT_EQUAL_UINT8(DIR_R, v.dir[0]);   // player 0 spawns heading right

  tick(g, STEP_TICKS - 1);
  v.read(g, 8, 8);
  TEST_ASSERT_EQUAL_INT(x0, v.hx[0]);         // not yet
  TEST_ASSERT_EQUAL_INT(y0, v.hy[0]);

  tick(g, 1);
  v.read(g, 8, 8);
  TEST_ASSERT_EQUAL_INT(x0 + 1, v.hx[0]);     // exactly one cell, on the tick
  TEST_ASSERT_EQUAL_INT(y0, v.hy[0]);
}

// Holding A halves the step interval.
void test_boost_doubles_the_step_rate(void) {
  Tron g;
  g.begin(8, 8, 0, 1, 1234, nullptr);
  View v;
  v.read(g, 8, 8);
  const int x0 = v.hx[0];

  hold(g, 0, TRON_NO_TURN, true);
  tick(g, BOOST_STEP_TICKS);
  v.read(g, 8, 8);
  TEST_ASSERT_EQUAL_INT(x0 + 1, v.hx[0]);     // moved in half the usual time

  tick(g, BOOST_STEP_TICKS);
  v.read(g, 8, 8);
  TEST_ASSERT_EQUAL_INT(x0 + 2, v.hx[0]);
}

// The trail is finite: past its maximum length the oldest cell is released, so
// the board does not simply fill up.
void test_trail_trims_to_max_length(void) {
  Tron g;
  g.begin(8, 8, 0, 1, 1234, nullptr);
  View v;

  tick(g, STEP_TICKS * 5);        // five steps: six cells visited
  v.read(g, 8, 8);

  TEST_ASSERT_TRUE(v.alive[0]);
  TEST_ASSERT_EQUAL_INT(START_LEN, v.trailCells(0));   // trimmed, not accumulated
}

// Maximum trail length grows with match time, which is what eventually forces
// players into each other instead of circling forever.
void test_max_trail_length_grows_over_time(void) {
  Tron g;
  g.begin(16, 16, 0, 1, 1234, nullptr);   // room to run long enough to grow
  View v;

  // Seven steps in: eight cells visited, exactly the 16x16 starting cap.
  tick(g, STEP_TICKS_16 * 7);             // tick 70, short of the first growth
  v.read(g, 16, 16);
  TEST_ASSERT_TRUE(v.alive[0]);
  TEST_ASSERT_EQUAL_INT(START_LEN_16, v.trailCells(0));

  tick(g, STEP_TICKS_16);                 // tick 80, past GROW_TICKS_16
  v.read(g, 16, 16);
  TEST_ASSERT_TRUE(v.alive[0]);
  TEST_ASSERT_EQUAL_INT(START_LEN_16 + 80 / GROW_TICKS_16, v.trailCells(0));
}

// The retune itself, pinned. Every number here is derived from the arena, so a
// change to the derivation shows up as a failure naming the panel it broke
// rather than as a match that quietly feels wrong on one of them.
void test_tuning_scales_with_the_arena(void) {
  Tron small, big;
  View v;

  small.begin(8, 8, 0, 1, 1234, nullptr);
  tick(small, STEP_TICKS);
  v.read(small, 8, 8);
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, v.hx[0], "8x8 step rate changed");

  // Half the ticks per step on the bigger board: one step in what 8x8 needs two.
  big.begin(16, 16, 0, 1, 1234, nullptr);
  tick(big, STEP_TICKS_16);
  v.read(big, 16, 16);
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, v.hx[0], "16x16 step rate changed");

  // And a longer starting trail, or a 256-cell board is empty enough to circle
  // in forever.
  tick(big, STEP_TICKS_16 * 6);
  v.read(big, 16, 16);
  TEST_ASSERT_EQUAL_INT(START_LEN_16, v.trailCells(0));
}

// ---- heading input ----------------------------------------------------------
// The rev 2 model: the player names a direction and goes there, instead of
// asking for a turn relative to wherever they happen to be pointing.

void test_requested_heading_is_adopted(void) {
  Tron g;
  g.begin(8, 8, 0, 1, 1234, nullptr);
  View v;
  v.read(g, 8, 8);
  const int x0 = v.hx[0], y0 = v.hy[0];

  hold(g, 0, DIR_D, false);
  tick(g, STEP_TICKS);
  v.read(g, 8, 8);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(DIR_D, v.dir[0], "the heading was not adopted");
  TEST_ASSERT_EQUAL_INT(x0, v.hx[0]);
  TEST_ASSERT_EQUAL_INT(y0 + 1, v.hy[0]);
}

void test_no_turn_keeps_going(void) {
  Tron g;
  g.begin(8, 8, 0, 1, 1234, nullptr);
  View v;
  v.read(g, 8, 8);
  const int x0 = v.hx[0];

  hold(g, 0, TRON_NO_TURN, false);
  tick(g, STEP_TICKS);
  v.read(g, 8, 8);
  TEST_ASSERT_EQUAL_UINT8(DIR_R, v.dir[0]);
  TEST_ASSERT_EQUAL_INT(x0 + 1, v.hx[0]);
}

// The one rule that has to live in the SIM rather than the UI. A reversal walks
// straight into your own neck, and the client asking for it is not trusted to
// have checked -- a hostile or just out-of-date unit must not be able to kill
// itself through a path the host accepted.
void test_reversal_is_rejected(void) {
  Tron g;
  g.begin(8, 8, 0, 1, 1234, nullptr);
  View v;
  v.read(g, 8, 8);
  const int x0 = v.hx[0], y0 = v.hy[0];
  TEST_ASSERT_EQUAL_UINT8(DIR_R, v.dir[0]);

  hold(g, 0, DIR_L, false);              // straight back into its own trail
  tick(g, STEP_TICKS);
  v.read(g, 8, 8);

  TEST_ASSERT_TRUE_MESSAGE(v.alive[0], "a reversal killed the player");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(DIR_R, v.dir[0], "the reversal was accepted");
  TEST_ASSERT_EQUAL_INT(x0 + 1, v.hx[0]);
  TEST_ASSERT_EQUAL_INT(y0, v.hy[0]);
}

// A quarter turn away from a reversal is legal -- only the exact opposite is
// refused, so the check must not be over-eager.
void test_perpendicular_turns_are_legal(void) {
  Tron g;
  g.begin(8, 8, 0, 1, 1234, nullptr);
  View v;

  hold(g, 0, DIR_U, false);
  tick(g, STEP_TICKS);
  v.read(g, 8, 8);
  TEST_ASSERT_EQUAL_UINT8(DIR_U, v.dir[0]);
}

// ---- stick -> heading -------------------------------------------------------

static uint8_t headingFor(Tron& g, int x, int y) {
  uint8_t buf[2] = { 0, 0 };
  LocalInput in{ (int16_t)x, (int16_t)y, false, false };
  g.serializeInput(buf, sizeof(buf), in);
  return buf[0];
}

void test_stick_resolves_to_the_dominant_axis(void) {
  Tron g;
  g.begin(8, 8, 0, 1, 1234, nullptr);

  TEST_ASSERT_EQUAL_UINT8(DIR_R, headingFor(g, 100, 30));
  TEST_ASSERT_EQUAL_UINT8(DIR_L, headingFor(g, -100, 30));
  TEST_ASSERT_EQUAL_UINT8(DIR_U, headingFor(g, 30, 100));    // +y is UP
  TEST_ASSERT_EQUAL_UINT8(DIR_D, headingFor(g, 30, -100));
}

// Below the threshold, and on an exact diagonal, the answer is "no request" --
// which the sim reads as "keep going". Guessing on a tie would drop the player
// somewhere they did not aim.
void test_stick_ambiguity_requests_nothing(void) {
  Tron g;
  g.begin(8, 8, 0, 1, 1234, nullptr);

  TEST_ASSERT_EQUAL_UINT8(TRON_NO_TURN, headingFor(g, 0, 0));
  TEST_ASSERT_EQUAL_UINT8(TRON_NO_TURN, headingFor(g, 20, 20));      // too small
  TEST_ASSERT_EQUAL_UINT8(TRON_NO_TURN, headingFor(g, 100, 100));    // exact tie
  TEST_ASSERT_EQUAL_UINT8(TRON_NO_TURN, headingFor(g, -80, 80));     // tie, other quadrant
}

// The AI drives through the same applyInput path as a human, so what it emits
// has to be a heading now, not the relative turn it used to convert back into.
void test_ai_emits_a_legal_heading(void) {
  Tron g;
  g.begin(8, 8, 0, 2, 1234, nullptr);

  uint8_t buf[2] = { 0, 0 };
  TEST_ASSERT_EQUAL_UINT(2, g.aiInput(1, buf, sizeof(buf)));
  TEST_ASSERT_TRUE_MESSAGE(buf[0] < 4, "the AI emitted something that is not a heading");
}

// Running into the arena edge doesn't kill on the first attempt: one step's
// grace holds the player at the wall instead, and only a second consecutive
// attempt (see test below for the alternative -- turning away) costs the
// match.
void test_wall_kills(void) {
  Tron g;
  g.begin(8, 8, 0, 1, 1234, nullptr);
  View v;
  v.read(g, 8, 8);
  const int x0 = v.hx[0];                 // heading right from x0

  tick(g, STEP_TICKS * (7 - x0));         // walk to the last column, still alive
  v.read(g, 8, 8);
  TEST_ASSERT_TRUE(v.alive[0]);
  TEST_ASSERT_EQUAL_INT(7, v.hx[0]);

  tick(g, STEP_TICKS);                    // one more step would be off the board -- held, not killed
  v.read(g, 8, 8);
  TEST_ASSERT_TRUE(v.alive[0]);
  TEST_ASSERT_EQUAL_INT(7, v.hx[0]);       // did not move

  tick(g, STEP_TICKS);                    // still facing the wall a step later: now it's fatal
  v.read(g, 8, 8);
  TEST_ASSERT_FALSE(v.alive[0]);
}

// The grace step exists to be used: turning away during it is a normal step
// in the new direction, not a second wall attempt, so it costs nothing.
void test_wall_grace_lets_you_turn_away(void) {
  Tron g;
  g.begin(8, 8, 0, 1, 1234, nullptr);
  View v;
  v.read(g, 8, 8);
  const int x0 = v.hx[0], y0 = v.hy[0];   // heading right from (x0, y0)

  tick(g, STEP_TICKS * (7 - x0));         // walk to the last column
  tick(g, STEP_TICKS);                    // held at the wall, grace consumed
  v.read(g, 8, 8);
  TEST_ASSERT_TRUE(v.alive[0]);
  TEST_ASSERT_EQUAL_INT(7, v.hx[0]);

  hold(g, 0, DIR_D, false);               // turn away before the next step
  tick(g, STEP_TICKS);
  v.read(g, 8, 8);
  TEST_ASSERT_TRUE(v.alive[0]);
  TEST_ASSERT_EQUAL_INT(7, v.hx[0]);
  TEST_ASSERT_EQUAL_INT(y0 + 1, v.hy[0]);
}

// Two heads entering the same cell on the same tick both die -- neither wins
// the exchange, and with nobody left the match is a draw.
void test_head_on_collision_kills_both(void) {
  Tron g;
  g.begin(8, 8, 0, 2, 1234, nullptr);

  // Two cells apart, facing each other: both target the cell between them.
  const int     hx[2]  = { 2, 4 };
  const int     hy[2]  = { 4, 4 };
  const uint8_t dir[2] = { DIR_R, DIR_L };
  placeHeads(g, 8, 8, 2, hx, hy, dir);

  tick(g, STEP_TICKS);

  View v;
  v.read(g, 8, 8);
  TEST_ASSERT_FALSE(v.alive[0]);
  TEST_ASSERT_FALSE(v.alive[1]);

  uint8_t winner = 0;
  TEST_ASSERT_TRUE(g.isOver(winner));
  TEST_ASSERT_EQUAL_UINT8(NET_PID_NONE, winner);   // nobody left: a draw
}

// Steering into a neighbour's trail is fatal, and the survivor takes the match.
void test_running_into_a_trail_kills_and_ends_the_match(void) {
  Tron g;
  g.begin(8, 8, 0, 2, 1234, nullptr);

  // Player 0 is one step from player 1's cell and heading straight at it.
  const int     hx[2]  = { 3, 4 };
  const int     hy[2]  = { 4, 4 };
  const uint8_t dir[2] = { DIR_R, DIR_U };
  placeHeads(g, 8, 8, 2, hx, hy, dir);

  tick(g, STEP_TICKS);

  View v;
  v.read(g, 8, 8);
  TEST_ASSERT_FALSE(v.alive[0]);      // drove into an occupied cell
  TEST_ASSERT_TRUE(v.alive[1]);

  uint8_t winner = 0;
  TEST_ASSERT_TRUE(g.isOver(winner));
  TEST_ASSERT_EQUAL_UINT8(1, winner);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_head_steps_once_per_step_interval);
  RUN_TEST(test_boost_doubles_the_step_rate);
  RUN_TEST(test_trail_trims_to_max_length);
  RUN_TEST(test_max_trail_length_grows_over_time);
  RUN_TEST(test_tuning_scales_with_the_arena);
  RUN_TEST(test_requested_heading_is_adopted);
  RUN_TEST(test_no_turn_keeps_going);
  RUN_TEST(test_reversal_is_rejected);
  RUN_TEST(test_perpendicular_turns_are_legal);
  RUN_TEST(test_stick_resolves_to_the_dominant_axis);
  RUN_TEST(test_stick_ambiguity_requests_nothing);
  RUN_TEST(test_ai_emits_a_legal_heading);
  RUN_TEST(test_wall_kills);
  RUN_TEST(test_wall_grace_lets_you_turn_away);
  RUN_TEST(test_head_on_collision_kills_both);
  RUN_TEST(test_running_into_a_trail_kills_and_ends_the_match);
  return UNITY_END();
}
