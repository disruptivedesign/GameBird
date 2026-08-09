// ============================================================================
//  Native tests for the Breakout simulation (src/games/breakout/breakout_sim.cpp).
//
//  Breakout is the first thing on this device whose behaviour is genuinely
//  hard to eyeball. A ball that passes through a brick at high speed, an angle
//  that has quietly gone flat, a paddle that catches a pixel early -- all of
//  them look like "the panel is small" rather than like a bug.
//
//  So the interesting suites here are the two at the bottom: a full screen
//  played out against a perfect paddle, run at BOTH 8x8 and 16x16, asserting
//  the invariants continuously. That pair is what makes the 16x16 upgrade a
//  configuration change rather than a bring-up.
//
//  Run with:  pio test -e native
// ============================================================================
#include <unity.h>
#include "games/breakout/breakout_sim.h"

// Mirrors of constants private to breakout_sim.cpp. Repeated here on purpose:
// if someone retunes the feel, these fail and say so rather than drifting.
static const int32_t BASE_SPEED_Q8 = 30;
static const int     MAX_SPEED_STEP = 3;
static const int     PAD_CROSS_TICKS = 50;   // ticks to cross the board at full tilt

// The speed-up schedule is now a FRACTION of the screen's bricks (a quarter and
// three quarters) rather than the fixed 4 and 12 hits it was, so that 16x16's 64
// bricks speed up at the same point in a screen as 8x8's 16.
static int speedupHit1(int w, int rows) { return w * rows / 4; }

// The horizontal fan, mirrored from UNIT[] in the sim.
static const int16_t UNIT_SX[BRK_NUM_DIRS] = { -232, -181,  -88,   88,  181,  232 };
static const int16_t UNIT_CY[BRK_NUM_DIRS] = {  108,  181,  241,  241,  181,  108 };

// ---- helpers ----------------------------------------------------------------

// Put the paddle under the ball. Teleports rather than steering, so this is a
// perfect player: it never misses, which is what lets a test play a whole screen
// out and assert on the result.
//
// placePaddle exists FOR this. The real input is a velocity now (steerPaddle),
// and driving a test's ideal player through an integrator would mean simulating
// travel time and turn-arounds -- which is a test of the paddle, not of the
// physics these suites are here to pin.
static void trackPaddle(BreakoutSim& s) {
  int padW   = s.paddleWidth();
  int maxCol = s.w() - padW;
  int target = s.ballCol() - padW / 2;
  if (target < 0)      target = 0;
  if (target > maxCol) target = maxCol;
  s.placePaddle(target);
}

// Punch a hole in one column so a test can reach the ceiling. Placing the ball
// inside a brick cell makes the X pass break exactly that brick.
static void clearColumn(BreakoutSim& s, int c) {
  for (int r = 0; r < s.brickRows(); r++) {
    s.placeBall(c * BRK_FP + BRK_FP / 2, r * BRK_FP + BRK_FP / 2, 3, true);
    s.step();
  }
}

// What a fully cleared screen is worth: rows score 1, 3, 5, 7 from the bottom.
static uint32_t screenValue(int w, int rows) {
  uint32_t total = 0;
  for (int tier = 0; tier < rows; tier++) total += (uint32_t)w * (1 + 2 * tier);
  return total;
}

// ---- geometry ---------------------------------------------------------------

// The whole 8x8 -> 16x16 story for the playfield is these two derivations.
void test_geometry_8x8(void) {
  BreakoutSim s;
  s.newGame(8, 8);
  TEST_ASSERT_EQUAL_INT(2, s.brickRows());
  TEST_ASSERT_EQUAL_INT(3, s.paddleWidth());
  TEST_ASSERT_EQUAL_INT(3, s.lives());
  for (int c = 0; c < 8; c++) {
    TEST_ASSERT_TRUE(s.brick(c, 0));
    TEST_ASSERT_TRUE(s.brick(c, 1));
    TEST_ASSERT_FALSE(s.brick(c, 2));
  }
}

void test_geometry_16x16(void) {
  BreakoutSim s;
  s.newGame(16, 16);
  TEST_ASSERT_EQUAL_INT(4, s.brickRows());
  TEST_ASSERT_EQUAL_INT(6, s.paddleWidth());
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 16; c++) TEST_ASSERT_TRUE(s.brick(c, r));
}

// Speed scales with height so the ball takes the same TIME to cross a taller
// board. Without this the 16x16 panel feels half as fast for free.
void test_speed_scales_with_height(void) {
  BreakoutSim a, b;
  a.newGame(8, 8);
  b.newGame(16, 16);
  TEST_ASSERT_EQUAL_INT32(BASE_SPEED_Q8, a.speed());
  TEST_ASSERT_EQUAL_INT32(BASE_SPEED_Q8 * 2, b.speed());
}

// ---- the two representation invariants --------------------------------------

// No direction is vertical and none is flat: every entry makes real progress on
// BOTH axes every tick. A ball that cannot represent a stuck angle cannot get
// stuck in one, which is why this is a property of the table and not a clamp.
void test_no_degenerate_angles(void) {
  for (int d = 0; d < BRK_NUM_DIRS; d++) {
    int32_t vx = (int32_t)UNIT_SX[d] * BASE_SPEED_Q8 / BRK_FP;
    int32_t vy = (int32_t)UNIT_CY[d] * BASE_SPEED_Q8 / BRK_FP;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, vx, "a direction has no horizontal motion");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, vy, "a direction has no vertical motion");
  }
}

// The collision pass does not sweep, so it is only free of tunnelling while a
// tick moves the ball less than one cell on each axis. This is the ceiling the
// whole model rests on -- check it at the fastest the game can ever get.
void test_speed_never_tunnels(void) {
  BreakoutSim s;
  s.newGame(16, 16);
  for (int i = 0; i < 40; i++) s.nextLevel();      // past the level-bonus cap
  for (int i = 0; i < MAX_SPEED_STEP + 2; i++) {   // and past the step cap
    clearColumn(s, 0);
    for (int k = 0; k < speedupHit1(s.w(), s.brickRows()) + 2 && !s.cleared(); k++) clearColumn(s, k + 1);
  }
  TEST_ASSERT_LESS_OR_EQUAL_INT32(BRK_MAX_SPEED, s.speed());
  for (int d = 0; d < BRK_NUM_DIRS; d++) {
    int32_t vx = (int32_t)UNIT_SX[d] * BRK_MAX_SPEED / BRK_FP;
    int32_t vy = (int32_t)UNIT_CY[d] * BRK_MAX_SPEED / BRK_FP;
    TEST_ASSERT_LESS_THAN_INT32_MESSAGE(BRK_FP, vx, "a tick can cross a whole cell");
    TEST_ASSERT_LESS_THAN_INT32_MESSAGE(BRK_FP, vy, "a tick can cross a whole cell");
  }
}

// ---- reflection -------------------------------------------------------------

void test_left_wall_mirrors_direction(void) {
  BreakoutSim s;
  s.newGame(8, 8);
  s.placeBall(5, 4 * BRK_FP, 0, false);       // hard left, in open space
  uint8_t ev = s.step();
  TEST_ASSERT_TRUE(ev & BRK_EV_WALL);
  TEST_ASSERT_EQUAL_INT(5, s.dir());          // 0 -> 5, mirrored
  TEST_ASSERT_TRUE(s.ballX() >= 0);
}

void test_right_wall_mirrors_direction(void) {
  BreakoutSim s;
  s.newGame(8, 8);
  s.placeBall(8 * BRK_FP - 5, 4 * BRK_FP, 5, false);
  uint8_t ev = s.step();
  TEST_ASSERT_TRUE(ev & BRK_EV_WALL);
  TEST_ASSERT_EQUAL_INT(0, s.dir());
  TEST_ASSERT_TRUE(s.ballX() < 8 * BRK_FP);
}

// Reaching the ceiling halves the paddle -- breaking through is a reward that
// immediately costs you.
void test_top_wall_shrinks_paddle_once(void) {
  BreakoutSim s;
  s.newGame(8, 8);
  clearColumn(s, 3);
  TEST_ASSERT_EQUAL_INT(3, s.paddleWidth());

  s.placeBall(3 * BRK_FP + BRK_FP / 2, 10, 3, true);
  uint8_t ev = s.step();
  TEST_ASSERT_TRUE(ev & BRK_EV_WALL);
  TEST_ASSERT_TRUE(ev & BRK_EV_SHRINK);
  TEST_ASSERT_FALSE(s.movingUp());
  TEST_ASSERT_EQUAL_INT(2, s.paddleWidth());

  // Only once per screen.
  s.placeBall(3 * BRK_FP + BRK_FP / 2, 10, 3, true);
  ev = s.step();
  TEST_ASSERT_FALSE(ev & BRK_EV_SHRINK);
  TEST_ASSERT_EQUAL_INT(2, s.paddleWidth());
}

// ---- the paddle -------------------------------------------------------------

void test_paddle_returns_the_ball(void) {
  BreakoutSim s;
  s.newGame(8, 8);
  s.placePaddle(0);                                   // paddle at columns 0..2
  s.placeBall(BRK_FP, 7 * BRK_FP - 1, 3, false);       // over it, falling
  uint8_t ev = s.step();
  TEST_ASSERT_TRUE(ev & BRK_EV_PADDLE);
  TEST_ASSERT_TRUE(s.movingUp());
  TEST_ASSERT_TRUE(s.ballY() < 7 * BRK_FP);
}

// The one mechanic that makes Breakout a game: WHERE it lands decides where it
// goes. Same incoming ball, opposite ends of the paddle, opposite outcomes.
void test_english_depends_on_where_it_lands(void) {
  BreakoutSim left, right;
  left.newGame(8, 8);
  right.newGame(8, 8);
  left.placePaddle(0);
  right.placePaddle(0);

  left.placeBall(100, 7 * BRK_FP - 1, 3, false);       // near the paddle's left
  right.placeBall(700, 7 * BRK_FP - 1, 3, false);      // near its right
  left.step();
  right.step();

  TEST_ASSERT_EQUAL_INT_MESSAGE(0, left.dir(),  "left edge should send it hard left");
  TEST_ASSERT_EQUAL_INT_MESSAGE(5, right.dir(), "right edge should send it hard right");
}

void test_missing_costs_a_life_and_reparks(void) {
  BreakoutSim s;
  s.newGame(8, 8);
  s.placePaddle(0);                                   // paddle at columns 0..2
  s.placeBall(7 * BRK_FP, 7 * BRK_FP - 1, 4, false);   // falling far from it

  uint8_t ev = 0;
  for (int i = 0; i < 200 && !(ev & BRK_EV_LOST); i++) ev = s.step();

  TEST_ASSERT_TRUE_MESSAGE(ev & BRK_EV_LOST, "the ball never went out");
  TEST_ASSERT_EQUAL_INT(2, s.lives());
  TEST_ASSERT_TRUE(s.parked());
  TEST_ASSERT_FALSE(s.gameOver());
}

// ---- bricks -----------------------------------------------------------------

// Deeper rows are worth more, which is what makes tunnelling up a side both
// the clever play and the scoring play.
void test_deeper_rows_score_more(void) {
  BreakoutSim bottom, top;
  bottom.newGame(8, 8);
  top.newGame(8, 8);

  bottom.placeBall(4 * BRK_FP + BRK_FP / 2, 1 * BRK_FP + BRK_FP / 2, 3, true);
  bottom.step();
  top.placeBall(4 * BRK_FP + BRK_FP / 2, 0 * BRK_FP + BRK_FP / 2, 3, true);
  top.step();

  TEST_ASSERT_EQUAL_UINT32(1, bottom.score());
  TEST_ASSERT_EQUAL_UINT32(3, top.score());
  TEST_ASSERT_FALSE(bottom.brick(4, 1));
  TEST_ASSERT_FALSE(top.brick(4, 0));
}

void test_brick_hit_reflects_and_speeds_up(void) {
  BreakoutSim s;
  s.newGame(8, 8);
  const int hits = speedupHit1(8, s.brickRows());        // 4 on an 8x8 screen
  int32_t before = s.speed();
  for (int c = 0; c < hits; c++) {
    s.placeBall(c * BRK_FP + BRK_FP / 2, 1 * BRK_FP + BRK_FP / 2, 3, true);
    s.step();
  }
  TEST_ASSERT_EQUAL_UINT32((uint32_t)hits, s.score());   // bottom-row bricks
  TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(before, s.speed(),
                                         "no speed-up at a quarter of the screen");
}

// The schedule has to land at the same POINT in a screen on both panels, which
// is the entire reason it stopped being an absolute hit count. Fixed at 4, a
// 16x16 screen would speed up after the first six percent of its bricks.
void test_speedup_schedule_scales_with_the_screen(void) {
  BreakoutSim s;
  s.newGame(16, 16);
  const int hits = speedupHit1(16, s.brickRows());       // 16 of 64
  TEST_ASSERT_EQUAL_INT(16, hits);

  int32_t before = s.speed();
  for (int c = 0; c < hits - 1; c++) {                   // one short of the mark
    s.placeBall((c % 16) * BRK_FP + BRK_FP / 2,
                (3 - c / 16) * BRK_FP + BRK_FP / 2, 3, true);
    s.step();
  }
  TEST_ASSERT_EQUAL_INT32_MESSAGE(before, s.speed(), "sped up too early");
}

// ---- the velocity paddle ----------------------------------------------------

// Deflection is a RATE now. Rev 1's absolute mapping cannot survive a stick that
// springs back: letting go mid-rally would snap the paddle to the middle of the
// board, which is exactly where you were trying not to be.
void test_paddle_holds_still_at_centre(void) {
  BreakoutSim s;
  s.newGame(16, 16);
  s.placePaddle(4);
  for (int i = 0; i < 200; i++) s.steerPaddle(0);
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, s.paddleCol(), "the paddle drifted on its own");
}

void test_paddle_full_deflection_crosses_in_the_budgeted_ticks(void) {
  BreakoutSim s;
  s.newGame(16, 16);
  s.placePaddle(0);
  for (int i = 0; i < PAD_CROSS_TICKS; i++) s.steerPaddle(100);
  TEST_ASSERT_EQUAL_INT_MESSAGE(s.w() - s.paddleWidth(), s.paddleCol(),
                                "full deflection did not cross the board in time");
}

// A light push has to move it slowly rather than not at all -- that sub-cell
// resolution is the whole reason _padPos is fixed point.
void test_paddle_small_deflection_moves_slowly(void) {
  BreakoutSim s;
  s.newGame(16, 16);
  s.placePaddle(0);

  for (int i = 0; i < 10; i++) s.steerPaddle(15);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.paddleCol(), "a nudge jumped a whole cell at once");

  for (int i = 0; i < 100; i++) s.steerPaddle(15);
  TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, s.paddleCol(), "a nudge never moved it at all");
}

void test_paddle_clamps_at_both_edges(void) {
  BreakoutSim s;
  s.newGame(16, 16);

  for (int i = 0; i < 200; i++) s.steerPaddle(-100);
  TEST_ASSERT_EQUAL_INT(0, s.paddleCol());

  for (int i = 0; i < 200; i++) s.steerPaddle(100);
  TEST_ASSERT_EQUAL_INT(s.w() - s.paddleWidth(), s.paddleCol());
}

// The paddle can be parked hard right when it halves. A clamp that only ran on
// the column would leave _padPos past the new limit and let it creep back out.
void test_paddle_stays_on_board_after_shrinking(void) {
  BreakoutSim s;
  s.newGame(8, 8);
  for (int i = 0; i < 200; i++) s.steerPaddle(100);     // hard right
  clearColumn(s, 3);
  s.placeBall(3 * BRK_FP + BRK_FP / 2, 10, 3, true);
  s.step();                                            // ceiling -> shrink

  TEST_ASSERT_EQUAL_INT(2, s.paddleWidth());
  TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(s.w() - s.paddleWidth(), s.paddleCol(),
                                        "the paddle hung off the right edge");
}

// ---- playing a whole screen -------------------------------------------------

// The real test. Play a full screen against a perfect paddle and assert the
// invariants on every single tick.
static void playOutScreen(int w, int h) {
  BreakoutSim s;
  s.newGame((uint8_t)w, (uint8_t)h);
  s.launch();

  bool cleared = false;
  for (long i = 0; i < 2000000L; i++) {
    trackPaddle(s);
    s.step();

    // The invariant every other collision decision rests on: destroying a
    // brick on entry means the ball is never standing inside one, which is
    // what makes "reflect, then test the cell we landed in" safe.
    TEST_ASSERT_FALSE_MESSAGE(s.brick(s.ballCol(), s.ballRow()),
                              "the ball is standing inside a brick");
    TEST_ASSERT_LESS_OR_EQUAL_INT32(BRK_MAX_SPEED, s.speed());
    TEST_ASSERT_TRUE(s.ballCol() >= 0 && s.ballCol() < w);
    TEST_ASSERT_TRUE(s.ballRow() >= 0 && s.ballRow() < h);

    if (s.cleared()) { cleared = true; break; }
  }

  TEST_ASSERT_TRUE_MESSAGE(cleared, "a perfect paddle never cleared the screen");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, s.lives(), "a perfect paddle still lost a ball");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(screenValue(w, s.brickRows()), s.score(),
                                   "a cleared screen did not score its full value");
}

void test_full_screen_8x8(void)   { playOutScreen(8, 8); }
void test_full_screen_16x16(void) { playOutScreen(16, 16); }

// Clearing a screen keeps the score and lives and puts the bricks back.
void test_next_level_refills(void) {
  BreakoutSim s;
  s.newGame(8, 8);
  s.launch();
  for (long i = 0; i < 2000000L && !s.cleared(); i++) { trackPaddle(s); s.step(); }
  TEST_ASSERT_TRUE(s.cleared());

  uint32_t carried = s.score();
  s.nextLevel();
  TEST_ASSERT_FALSE(s.cleared());
  TEST_ASSERT_EQUAL_UINT32(carried, s.score());
  TEST_ASSERT_EQUAL_INT(3, s.lives());
  TEST_ASSERT_EQUAL_INT(1, s.level());
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, s.paddleWidth(), "a new screen restores the paddle");
  TEST_ASSERT_TRUE(s.parked());
  TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(BASE_SPEED_Q8, s.speed(),
                                         "a new level is not faster");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_geometry_8x8);
  RUN_TEST(test_geometry_16x16);
  RUN_TEST(test_speed_scales_with_height);
  RUN_TEST(test_no_degenerate_angles);
  RUN_TEST(test_speed_never_tunnels);
  RUN_TEST(test_left_wall_mirrors_direction);
  RUN_TEST(test_right_wall_mirrors_direction);
  RUN_TEST(test_top_wall_shrinks_paddle_once);
  RUN_TEST(test_paddle_returns_the_ball);
  RUN_TEST(test_english_depends_on_where_it_lands);
  RUN_TEST(test_missing_costs_a_life_and_reparks);
  RUN_TEST(test_deeper_rows_score_more);
  RUN_TEST(test_brick_hit_reflects_and_speeds_up);
  RUN_TEST(test_speedup_schedule_scales_with_the_screen);
  RUN_TEST(test_paddle_holds_still_at_centre);
  RUN_TEST(test_paddle_full_deflection_crosses_in_the_budgeted_ticks);
  RUN_TEST(test_paddle_small_deflection_moves_slowly);
  RUN_TEST(test_paddle_clamps_at_both_edges);
  RUN_TEST(test_paddle_stays_on_board_after_shrinking);
  RUN_TEST(test_full_screen_8x8);
  RUN_TEST(test_full_screen_16x16);
  RUN_TEST(test_next_level_refills);
  return UNITY_END();
}

void setUp(void)    {}
void tearDown(void) {}
