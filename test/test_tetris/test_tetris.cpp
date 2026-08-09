// ============================================================================
//  Native tests for the Tetris simulation (src/games/tetris/tetris_sim.cpp).
//
//  Tetris shipped and worked for a long time with none of this, so these tests
//  are not chasing a bug -- they are pinning the parts that are invisible from
//  the outside. A 7-bag that has quietly become uniform random still looks like
//  Tetris. A lock delay that fires a frame early still looks like Tetris. A
//  scoring multiplier that dropped the level term still looks like Tetris. Each
//  of those is a feel regression you would argue about rather than see.
//
//  Everything is driven by explicit dt and an explicit seed, so no clock and no
//  hardware is involved. Run with:  pio test -e native
// ============================================================================
#include <unity.h>
#include "games/tetris/tetris_sim.h"

// Mirrors of constants private to tetris_sim.cpp. Repeated on purpose: retuning
// the feel should make these fail and say so rather than drift silently.
static const uint32_t FALL_START_MS    = 900;
static const uint32_t SOFTDROP_MS      = 60;
static const uint32_t LOCK_DELAY_MS    = 200;
static const int      LINES_PER_LEVEL  = 4;
static const uint32_t LEVEL_SPEEDUP_MS = 100;
static const uint32_t DAS_DELAY_MS     = 170;
static const uint32_t ARR_MS           = 50;
static const int      TYPE_I = 0, TYPE_O = 1, TYPE_T = 2;

// Tests that place a piece and want it to STAY there call setMove(0): with DAS,
// no held direction means no horizontal movement at all. Rev 1 had to pin the
// slider to an edge instead, because an absolute target always pulled the piece
// somewhere.

// ---- helpers ----------------------------------------------------------------

static void clearBoard(TetrisSim& s) {
  for (int r = 0; r < s.h(); r++)
    for (int c = 0; c < s.w(); c++) s.placeCell(c, r, -1);
}

static int filledCells(TetrisSim& s) {
  int n = 0;
  for (int r = 0; r < s.h(); r++)
    for (int c = 0; c < s.w(); c++)
      if (s.cell(c, r) >= 0) n++;
  return n;
}

static void fillRow(TetrisSim& s, int row, int fromCol, int toCol) {
  for (int c = fromCol; c <= toCol; c++) s.placeCell(c, row, TYPE_O);
}

// Run until the active piece locks and its successor has spawned.
static void dropPiece(TetrisSim& s) {
  for (int i = 0; i < 4000; i++) {
    uint8_t ev = s.update(50);
    if (ev & TET_EV_LINES) s.commitClear();
    if (ev & TET_EV_LOCK)  return;
    if (s.gameOver())      return;
  }
  TEST_FAIL_MESSAGE("a piece never locked");
}

// Run until the sim reports something, and hand back what it reported.
static uint8_t runUntilEvent(TetrisSim& s, uint8_t mask) {
  for (int i = 0; i < 4000; i++) {
    uint8_t ev = s.update(20);
    if (ev & mask) return ev;
  }
  TEST_FAIL_MESSAGE("the expected event never fired");
  return 0;
}

// ---- geometry ---------------------------------------------------------------

void test_board_is_sized_from_begin(void) {
  TetrisSim a, b;
  a.newGame(8, 8, 1);
  b.newGame(16, 16, 1);
  TEST_ASSERT_EQUAL_INT(8,  a.w());
  TEST_ASSERT_EQUAL_INT(16, b.w());
  TEST_ASSERT_EQUAL_INT(16, b.h());
  // The spawn column centers itself rather than being the hard-coded 2 it was.
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, a.pieceX(), "8-wide spawn is off centre");
  TEST_ASSERT_EQUAL_INT_MESSAGE(6, b.pieceX(), "16-wide spawn is off centre");
}

// ---- the 7-bag --------------------------------------------------------------

// The whole point of a bag over uniform random: you cannot be starved of a
// piece for long. Uniform random passes no version of this test.
void test_bag_deals_each_piece_once_per_seven(void) {
  TetrisSim s;
  s.newGame(8, 8, 12345);

  int counts[TET_NUM_TYPES] = { 0 };
  counts[s.pieceType()]++;
  for (int i = 0; i < TET_NUM_TYPES - 1; i++) {
    clearBoard(s);                 // keep the well empty so nothing tops out
    dropPiece(s);
    counts[s.pieceType()]++;
  }
  for (int t = 0; t < TET_NUM_TYPES; t++)
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, counts[t], "a piece was missing from the bag");
}

void test_two_bags_deal_each_piece_twice(void) {
  TetrisSim s;
  s.newGame(8, 8, 999);

  int counts[TET_NUM_TYPES] = { 0 };
  counts[s.pieceType()]++;
  for (int i = 0; i < TET_NUM_TYPES * 2 - 1; i++) {
    clearBoard(s);
    dropPiece(s);
    counts[s.pieceType()]++;
  }
  for (int t = 0; t < TET_NUM_TYPES; t++)
    TEST_ASSERT_EQUAL_INT(2, counts[t]);
}

// Same seed, same sequence -- which is what makes every test above reproducible.
void test_same_seed_deals_the_same_bag(void) {
  TetrisSim a, b;
  a.newGame(8, 8, 4242);
  b.newGame(8, 8, 4242);
  for (int i = 0; i < TET_NUM_TYPES; i++) {
    TEST_ASSERT_EQUAL_INT(a.pieceType(), b.pieceType());
    clearBoard(a); clearBoard(b);
    dropPiece(a);  dropPiece(b);
  }
}

// ---- gravity and lock delay -------------------------------------------------

void test_gravity_drops_one_row_per_interval(void) {
  TetrisSim s;
  s.newGame(8, 8, 1);
  s.placePiece(TYPE_T, 0, 2, 0);
  int y0 = s.pieceY();

  s.update(FALL_START_MS - 1);
  TEST_ASSERT_EQUAL_INT_MESSAGE(y0, s.pieceY(), "fell early");
  s.update(1);
  TEST_ASSERT_EQUAL_INT_MESSAGE(y0 + 1, s.pieceY(), "did not fall on the interval");
}

// Gravity must not depend on how long a frame takes. The same elapsed time fed
// in one lump or in awkward slices has to drop the same number of rows -- which
// is only true if the accumulator carries its remainder. Frame time stopped
// being a constant when the panel quadrupled, so this is now a real variable
// rather than a theoretical one.
void test_gravity_is_frame_rate_independent(void) {
  TetrisSim coarse, fine;
  coarse.newGame(8, 16, 1);
  fine.newGame(8, 16, 1);
  coarse.setMove(0);
  fine.setMove(0);
  coarse.placePiece(TYPE_T, 0, 2, 0);
  fine.placePiece(TYPE_T, 0, 2, 0);

  // Five intervals' worth, one lump versus 13 ms slices that divide it badly.
  const uint32_t total = FALL_START_MS * 5;
  coarse.update(FALL_START_MS);
  for (int i = 1; i < 5; i++) coarse.update(FALL_START_MS);
  for (uint32_t t = 0; t < total; t += 13) fine.update(13);

  TEST_ASSERT_EQUAL_INT_MESSAGE(coarse.pieceY(), fine.pieceY(),
                                "fall rate changed with the frame time");
}

// ...but a stall must not bank rows and then spend them all at once.
void test_a_long_stall_drops_one_row_not_several(void) {
  TetrisSim s;
  s.newGame(8, 16, 1);
  s.setMove(0);
  s.placePiece(TYPE_T, 0, 2, 0);
  const int y0 = s.pieceY();

  s.update(FALL_START_MS * 6);        // one enormous frame
  TEST_ASSERT_EQUAL_INT_MESSAGE(y0 + 1, s.pieceY(), "a stall teleported the piece");

  s.update(1);                        // and the banked time is gone
  TEST_ASSERT_EQUAL_INT(y0 + 1, s.pieceY());
}

void test_soft_drop_is_faster_than_gravity(void) {
  TetrisSim s;
  s.newGame(8, 8, 1);
  s.placePiece(TYPE_T, 0, 2, 0);
  int y0 = s.pieceY();

  s.softDrop(true, true);
  s.update(SOFTDROP_MS);
  TEST_ASSERT_EQUAL_INT_MESSAGE(y0 + 1, s.pieceY(), "soft drop was not faster");
}

// Releasing B has to disarm it, or a held button would carry between pieces.
void test_soft_drop_disarms_on_release(void) {
  TetrisSim s;
  s.newGame(8, 8, 1);
  s.placePiece(TYPE_T, 0, 2, 0);
  int y0 = s.pieceY();

  s.softDrop(true, true);
  s.softDrop(false, false);          // let go
  s.update(SOFTDROP_MS);
  TEST_ASSERT_EQUAL_INT_MESSAGE(y0, s.pieceY(), "still soft-dropping after release");
}

// The grace period that lets you slide a piece under an overhang after landing.
void test_lock_delay_holds_before_committing(void) {
  TetrisSim s;
  s.newGame(8, 8, 1);
  s.setMove(0);
  s.placePiece(TYPE_O, 0, 2, 5);     // resting on the floor

  s.update(10);                      // becomes grounded
  s.update(LOCK_DELAY_MS / 2);
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, s.cell(3, 7), "locked before the delay elapsed");

  s.update(LOCK_DELAY_MS);
  TEST_ASSERT_EQUAL_INT_MESSAGE(TYPE_O, s.cell(3, 7), "never locked");
  TEST_ASSERT_EQUAL_INT(TYPE_O, s.cell(4, 7));
}

// ---- DAS --------------------------------------------------------------------
// The two-rate shape is the point: a flick has to be exactly one cell, and a
// hold must not start sliding until the player has clearly committed. Get either
// wrong and the game is still recognisably Tetris but no longer placeable on
// purpose -- precisely the kind of regression nobody can bisect by feel.

void test_tap_moves_exactly_one_cell(void) {
  TetrisSim s;
  s.newGame(8, 16, 1);
  s.placePiece(TYPE_O, 0, 3, 2);

  s.setMove(-1);
  s.update(16);
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, s.pieceX(), "a tap did not step once");

  // Still held, but well inside the DAS delay: nothing more may happen.
  s.update(DAS_DELAY_MS - 40);
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, s.pieceX(), "repeat started before the delay");
}

void test_release_and_tap_steps_again(void) {
  TetrisSim s;
  s.newGame(8, 16, 1);
  s.placePiece(TYPE_O, 0, 3, 2);

  s.setMove(-1); s.update(16);
  s.setMove(0);  s.update(16);
  s.setMove(-1); s.update(16);
  TEST_ASSERT_EQUAL_INT(1, s.pieceX());
}

void test_hold_repeats_at_the_arr_rate(void) {
  TetrisSim s;
  s.newGame(16, 16, 1);
  s.placePiece(TYPE_O, 0, 10, 2);

  s.setMove(-1);
  s.update(16);                                   // the tap
  TEST_ASSERT_EQUAL_INT(9, s.pieceX());

  s.update(DAS_DELAY_MS);                         // delay elapses -> one repeat
  TEST_ASSERT_EQUAL_INT_MESSAGE(8, s.pieceX(), "the first repeat did not fire");

  s.update(ARR_MS * 3);                           // then one per ARR
  TEST_ASSERT_EQUAL_INT_MESSAGE(5, s.pieceX(), "repeat rate is not ARR");
}

// Reversing has to tap at once rather than inherit the old direction's repeat
// phase -- otherwise a correction after a long slide either stalls for a beat or
// bolts the other way.
void test_reversing_direction_taps_immediately(void) {
  TetrisSim s;
  s.newGame(16, 16, 1);
  s.placePiece(TYPE_O, 0, 10, 2);

  s.setMove(-1);
  s.update(16);
  s.update(DAS_DELAY_MS + ARR_MS * 2);            // deep into the repeat
  int x = s.pieceX();

  s.setMove(+1);
  s.update(16);
  TEST_ASSERT_EQUAL_INT_MESSAGE(x + 1, s.pieceX(), "reversal did not step at once");
}

// Asserted as "it stops moving" rather than as a specific column, because where
// a piece runs out of room depends on where its cells sit inside its 4x4 box --
// an O never reaches box-x 0. The invariant that matters is that holding into a
// wall settles instead of walking through it.
void test_movement_stops_at_the_wall(void) {
  TetrisSim s;
  s.newGame(8, 16, 1);
  s.placePiece(TYPE_O, 0, 3, 2);

  s.setMove(-1);
  s.update(16);
  s.update(DAS_DELAY_MS + ARR_MS * 20);
  const int settled = s.pieceX();

  s.update(ARR_MS * 20);
  TEST_ASSERT_EQUAL_INT_MESSAGE(settled, s.pieceX(), "the piece walked through the wall");
}

// ---- hard drop --------------------------------------------------------------

void test_hard_drop_lands_on_the_ghost_and_locks(void) {
  TetrisSim s;
  s.newGame(8, 16, 1);
  s.setMove(0);
  clearBoard(s);
  s.placePiece(TYPE_O, 0, 3, 2);

  const uint8_t ev = s.hardDrop();

  TEST_ASSERT_TRUE(ev & TET_EV_HARDDROP);
  TEST_ASSERT_TRUE_MESSAGE(ev & TET_EV_LOCK, "a hard drop must lock, not hover");
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, filledCells(s), "the piece did not land whole");

  // Dropped into an empty well, so it can only have come to rest on the floor.
  bool onFloor = false;
  for (int c = 0; c < s.w(); c++) if (s.cell(c, s.h() - 1) >= 0) onFloor = true;
  TEST_ASSERT_TRUE_MESSAGE(onFloor, "it stopped short of the floor");
}

void test_hard_drop_scores_one_per_row(void) {
  TetrisSim s;
  s.newGame(8, 16, 1);
  s.setMove(0);
  clearBoard(s);
  s.placePiece(TYPE_O, 0, 3, 2);

  const uint32_t before = s.score();
  const int rows = s.ghostY() - s.pieceY();
  s.hardDrop();
  TEST_ASSERT_EQUAL_UINT32(before + (uint32_t)rows, s.score());
}

// Inert while a clear is latched, like every other input: the board is frozen
// waiting for the caller's animation, and a slam into it would commit a piece
// into rows that are about to collapse.
void test_hard_drop_is_ignored_mid_clear(void) {
  TetrisSim s;
  s.newGame(8, 8, 1);
  s.setMove(0);
  fillRow(s, 7, 1, 7);
  s.placePiece(TYPE_I, 1, -2, 4);

  runUntilEvent(s, TET_EV_LINES);
  TEST_ASSERT_TRUE(s.clearPending());
  TEST_ASSERT_EQUAL_UINT8(0, s.hardDrop());
}

// ---- rotation ---------------------------------------------------------------

// A rotation that would clip the wall is nudged inward instead of refused --
// without kicks, an I piece against the left edge simply cannot be laid flat.
void test_rotation_kicks_off_the_wall(void) {
  TetrisSim s;
  s.newGame(8, 8, 1);
  s.placePiece(TYPE_I, 1, -2, 2);    // vertical, hugging the left wall

  TEST_ASSERT_TRUE(s.rotate(+1) & TET_EV_ROTATE);
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, s.pieceRot(), "rotation was refused");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.pieceX(), "did not kick clear of the wall");
}

// The joystick button rotates the other way now that soft drop moved onto the
// stick. Turning one step each way from the same start has to land on
// opposite rotations.
void test_rotation_goes_both_ways(void) {
  TetrisSim s;
  s.newGame(8, 16, 1);

  s.placePiece(TYPE_T, 0, 3, 4);
  TEST_ASSERT_TRUE(s.rotate(+1) & TET_EV_ROTATE);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.pieceRot(), "A did not rotate clockwise");

  s.placePiece(TYPE_T, 0, 3, 4);
  TEST_ASSERT_TRUE(s.rotate(-1) & TET_EV_ROTATE);
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, s.pieceRot(), "joystick button did not rotate anticlockwise");
}

// The lookahead must be the piece that actually arrives next, or the panel is
// lying to the player about what to plan for.
void test_next_type_is_what_spawns(void) {
  TetrisSim s;
  s.newGame(8, 16, 12345);

  for (int i = 0; i < 20; i++) {
    const int predicted = s.nextType();
    // Drop the current piece out of the way; the spawn that follows must match.
    s.hardDrop();
    if (s.gameOver()) break;
    if (s.clearPending()) s.commitClear();
    TEST_ASSERT_EQUAL_INT_MESSAGE(predicted, s.pieceType(),
                                  "the info panel promised a different piece");
  }
}

// ---- clearing ---------------------------------------------------------------

void test_single_line_clears_and_scores(void) {
  TetrisSim s;
  s.newGame(8, 8, 1);
  s.setMove(0);
  fillRow(s, 7, 1, 7);                     // everything but column 0
  s.placePiece(TYPE_I, 1, -2, 4);          // vertical I filling that column

  uint8_t ev = runUntilEvent(s, TET_EV_LINES);
  TEST_ASSERT_TRUE(ev & TET_EV_LINES);
  TEST_ASSERT_EQUAL_INT(1, s.clearCount());
  TEST_ASSERT_TRUE_MESSAGE(s.clearRow(7), "the wrong row was latched");
  TEST_ASSERT_TRUE_MESSAGE(s.clearPending(), "the sim should be frozen mid-clear");

  s.commitClear();
  TEST_ASSERT_EQUAL_INT(1, s.lines());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, s.score(), "a single at level 0 is 1 point");
  TEST_ASSERT_FALSE(s.clearPending());
}

// Nothing advances while a clear waits to be committed -- that is what lets the
// caller hold the animation for as long as it likes.
void test_sim_is_frozen_until_the_clear_is_committed(void) {
  TetrisSim s;
  s.newGame(8, 8, 1);
  s.setMove(0);
  fillRow(s, 7, 1, 7);
  s.placePiece(TYPE_I, 1, -2, 4);
  runUntilEvent(s, TET_EV_LINES);

  int y = s.pieceY();
  for (int i = 0; i < 20; i++) TEST_ASSERT_EQUAL_UINT8(0, s.update(100));
  TEST_ASSERT_EQUAL_INT(y, s.pieceY());
  TEST_ASSERT_EQUAL_INT(1, s.clearCount());
}

// Four rows at once, and the reason to stack instead of keeping tidy: it is
// worth eight times a single, before the level multiplier even applies.
void test_four_rows_score_and_level_up(void) {
  TetrisSim s;
  s.newGame(8, 8, 1);
  s.setMove(0);
  for (int r = 4; r <= 7; r++) fillRow(s, r, 1, 7);
  s.placePiece(TYPE_I, 1, -2, 4);          // fills column 0 across all four

  runUntilEvent(s, TET_EV_LINES);
  TEST_ASSERT_EQUAL_INT(4, s.clearCount());

  uint32_t before = s.fallInterval();
  uint8_t ev = s.commitClear();

  TEST_ASSERT_EQUAL_UINT32_MESSAGE(8, s.score(), "a four-row clear is 8 points at level 0");
  TEST_ASSERT_EQUAL_INT(LINES_PER_LEVEL, s.lines());
  TEST_ASSERT_TRUE_MESSAGE(ev & TET_EV_LEVEL, "four lines should be a level");
  TEST_ASSERT_EQUAL_INT(1, s.level());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(before - LEVEL_SPEEDUP_MS, s.fallInterval(),
                                   "the level did not speed gravity up");
}

// The level multiplier is the part that is easy to drop in a refactor and
// impossible to notice: the same clear is worth more later in the game.
void test_score_scales_with_level(void) {
  TetrisSim s;
  s.newGame(8, 8, 1);
  s.setMove(0);

  // First: four rows -> 8 points, and level 1.
  for (int r = 4; r <= 7; r++) fillRow(s, r, 1, 7);
  s.placePiece(TYPE_I, 1, -2, 4);
  runUntilEvent(s, TET_EV_LINES);
  s.commitClear();
  TEST_ASSERT_EQUAL_INT(1, s.level());
  uint32_t after_first = s.score();

  // Then the same four rows again, now at level 1 -> 8 * 2.
  clearBoard(s);
  for (int r = 4; r <= 7; r++) fillRow(s, r, 1, 7);
  s.placePiece(TYPE_I, 1, -2, 4);
  runUntilEvent(s, TET_EV_LINES);
  s.commitClear();
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(after_first + 16, s.score(),
                                   "the level multiplier is missing");
}

void test_full_row_on_a_16_wide_board_clears(void) {
  TetrisSim s;
  s.newGame(16, 16, 1);
  s.setMove(0);
  fillRow(s, 15, 1, 15);                   // 16-wide row, one gap
  s.placePiece(TYPE_I, 1, -2, 12);

  runUntilEvent(s, TET_EV_LINES);
  TEST_ASSERT_EQUAL_INT(1, s.clearCount());
  TEST_ASSERT_TRUE(s.clearRow(15));
}

// ---- topping out -----------------------------------------------------------

void test_game_over_when_a_spawn_has_nowhere_to_go(void) {
  TetrisSim s;
  s.newGame(8, 8, 1);
  s.setMove(0);
  // Block the spawn area but leave column 7 open, so no row counts as full and
  // the pile is what ends the game rather than a clear.
  for (int r = 0; r <= 2; r++) fillRow(s, r, 0, 6);
  s.placePiece(TYPE_I, 1, 5, 4);           // vertical I down the open column

  uint8_t ev = runUntilEvent(s, TET_EV_GAMEOVER);
  TEST_ASSERT_TRUE(ev & TET_EV_GAMEOVER);
  TEST_ASSERT_TRUE(s.gameOver());

  // And it stays over: no further update does anything.
  TEST_ASSERT_EQUAL_UINT8(0, s.update(1000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_board_is_sized_from_begin);
  RUN_TEST(test_bag_deals_each_piece_once_per_seven);
  RUN_TEST(test_two_bags_deal_each_piece_twice);
  RUN_TEST(test_same_seed_deals_the_same_bag);
  RUN_TEST(test_gravity_drops_one_row_per_interval);
  RUN_TEST(test_gravity_is_frame_rate_independent);
  RUN_TEST(test_a_long_stall_drops_one_row_not_several);
  RUN_TEST(test_soft_drop_is_faster_than_gravity);
  RUN_TEST(test_soft_drop_disarms_on_release);
  RUN_TEST(test_lock_delay_holds_before_committing);
  RUN_TEST(test_tap_moves_exactly_one_cell);
  RUN_TEST(test_release_and_tap_steps_again);
  RUN_TEST(test_hold_repeats_at_the_arr_rate);
  RUN_TEST(test_reversing_direction_taps_immediately);
  RUN_TEST(test_movement_stops_at_the_wall);
  RUN_TEST(test_hard_drop_lands_on_the_ghost_and_locks);
  RUN_TEST(test_hard_drop_scores_one_per_row);
  RUN_TEST(test_hard_drop_is_ignored_mid_clear);
  RUN_TEST(test_rotation_kicks_off_the_wall);
  RUN_TEST(test_rotation_goes_both_ways);
  RUN_TEST(test_next_type_is_what_spawns);
  RUN_TEST(test_single_line_clears_and_scores);
  RUN_TEST(test_sim_is_frozen_until_the_clear_is_committed);
  RUN_TEST(test_four_rows_score_and_level_up);
  RUN_TEST(test_score_scales_with_level);
  RUN_TEST(test_full_row_on_a_16_wide_board_clears);
  RUN_TEST(test_game_over_when_a_spawn_has_nowhere_to_go);
  return UNITY_END();
}

void setUp(void)    {}
void tearDown(void) {}
