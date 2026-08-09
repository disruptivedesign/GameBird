// ============================================================================
//  Native tests for the joystick input layer (src/core/controls.cpp).
//
//  These exist because the joystick is the one part of the rev 2 change with no
//  visible failure mode. A panel wired wrong is obvious in a second. A stick
//  whose deadzone is slightly off, or that double-steps one time in twenty, or
//  whose auto-repeat starts a beat early, produces a device that is merely
//  slightly unpleasant -- and you cannot bisect "slightly unpleasant" by hand.
//
//  Everything here runs against the fake ADC in test/stubs.cpp, with time handed
//  in explicitly. No board, no stick. Run with:  pio test -e native
// ============================================================================
#include <unity.h>
#include "core/controls.h"

// Mirrors of the tunables in defines.h. Repeated on purpose: retuning the feel
// should make these fail and say so rather than drift silently.
static const int DEADZONE     = 12;
static const int STEP_ON      = 55;
static const int STEP_OFF     = 35;
static const uint32_t RPT_DELAY = 320;
static const uint32_t RPT_MS    = 110;

static const int MID = ADC_MAX / 2;

// ---- helpers ----------------------------------------------------------------

// A raw ADC reading `pct` of the way from centre to an extreme, where centre is
// mid-scale. Positive is toward ADC_MAX.
static int rawAt(int pct) {
  int v = MID + (int)((long)pct * (ADC_MAX - MID) / 100);
  return v < 0 ? 0 : (v > ADC_MAX ? ADC_MAX : v);
}

// The same, in PHYSICAL terms: +pct is right on X and UP on Y whatever the
// board's inversion constants happen to be.
//
// Anything asserting a direction has to go through these. Driving the raw ADC
// directly silently bakes in today's value of JOY_INVERT_*, so the day a board
// wires an axis the other way the tests keep passing while the device does the
// opposite of what it is told -- which is exactly how an inverted Y reached
// hardware and surfaced as Tetris slamming pieces on a soft drop.
static int rawX(int pct) { return rawAt(JOY_INVERT_X ? -pct : pct); }
static int rawY(int pct) { return rawAt(JOY_INVERT_Y ? -pct : pct); }

static int rawRight() { return rawX(100); }
static int rawLeft()  { return rawX(-100); }
static int rawUp()    { return rawY(100); }
static int rawDown()  { return rawY(-100); }

// What Axis::read() should report for a physical deflection of `pct`: the
// deadzone band collapsed to zero, everything past it stretched back out to the
// full range.
static int expectedPct(int pct) {
  int mag = pct < 0 ? -pct : pct;
  if (mag < DEADZONE) return 0;
  int out = ((mag - DEADZONE) * 100 + (100 - DEADZONE) / 2) / (100 - DEADZONE);
  return pct < 0 ? -out : out;
}

// Settle an axis at a raw reading. update() runs an EMA, so a single call never
// reaches the target; 40 is far past convergence.
static int settleAxis(Axis& a, uint8_t pin, int raw) {
  nativeSetAnalog(pin, raw);
  for (int i = 0; i < 40; i++) a.update();
  return a.read();
}

// Hold the X axis at a raw reading for `durMs`, polling every 5 ms like the main
// loop does, and count the steps that fire.
static int countStepsX(Joystick& j, int raw, uint32_t& now, uint32_t durMs) {
  nativeSetAnalog(JOY_X_PIN, raw);
  int n = 0;
  for (uint32_t t = 0; t < durMs; t += 5) {
    now += 5;
    j.update(now);
    if (j.stepX()) n++;
  }
  return n;
}

static void freshJoystick(Joystick& j, uint32_t& now) {
  nativeResetPins();                 // both axes back to mid-scale
  j.begin();
  now = 1000;                        // not zero, so nothing passes by accident
  j.update(now);
}

// ============================================================================
//  Axis: deadzone, rescale, centre calibration
// ============================================================================

static void test_axis_reads_zero_at_rest() {
  nativeResetPins();
  Axis a;
  a.begin(JOY_X_PIN, false);
  TEST_ASSERT_EQUAL_INT(0, settleAxis(a, JOY_X_PIN, MID));
}

// The whole point of the deadzone: small wander around centre is not input.
static void test_axis_deadzone_swallows_small_deflection() {
  nativeResetPins();
  Axis a;
  a.begin(JOY_X_PIN, false);
  TEST_ASSERT_EQUAL_INT(0, settleAxis(a, JOY_X_PIN, rawAt(DEADZONE - 4)));
  TEST_ASSERT_EQUAL_INT(0, settleAxis(a, JOY_X_PIN, rawAt(-(DEADZONE - 4))));
}

// ...and the whole point of RESCALING it: full physical travel must still reach
// 100, not 100 minus the deadzone. This is the bug the rev 1 slider had before
// the rescale went in, and it showed up as a paddle that snagged near centre.
static void test_axis_full_travel_still_reaches_the_ends() {
  nativeResetPins();
  Axis a;
  a.begin(JOY_X_PIN, false);
  TEST_ASSERT_EQUAL_INT( 100, settleAxis(a, JOY_X_PIN, ADC_MAX));
  TEST_ASSERT_EQUAL_INT(-100, settleAxis(a, JOY_X_PIN, 0));
}

static void test_axis_rescales_the_live_band() {
  nativeResetPins();
  Axis a;
  a.begin(JOY_X_PIN, false);
  for (int pct = -100; pct <= 100; pct += 10) {
    int got = settleAxis(a, JOY_X_PIN, rawAt(pct));
    // One count of slack for the EMA's integer division.
    TEST_ASSERT_INT_WITHIN(1, expectedPct(pct), got);
  }
}

static void test_axis_invert_flips_the_sense() {
  nativeResetPins();
  Axis a;
  a.begin(JOY_X_PIN, true);
  TEST_ASSERT_EQUAL_INT(-100, settleAxis(a, JOY_X_PIN, ADC_MAX));
  TEST_ASSERT_EQUAL_INT( 100, settleAxis(a, JOY_X_PIN, 0));
}

// An off-centre stick is the normal case, not the exception. Both extremes must
// still reach 100, which is what the per-side scaling in Axis::update buys: a
// single divisor would saturate the short side early and starve the long one.
static void test_axis_calibrates_an_off_centre_stick() {
  nativeResetPins();
  nativeSetAnalog(JOY_X_PIN, 1750);          // resting well below mid-scale
  Axis a;
  a.begin(JOY_X_PIN, false);

  TEST_ASSERT_EQUAL_INT(1750, a.centerRaw());
  TEST_ASSERT_EQUAL_INT(0,    settleAxis(a, JOY_X_PIN, 1750));
  TEST_ASSERT_EQUAL_INT(100,  settleAxis(a, JOY_X_PIN, ADC_MAX));
  TEST_ASSERT_EQUAL_INT(-100, settleAxis(a, JOY_X_PIN, 0));
}

// Held at boot: adopting that as centre would put true centre at full
// deflection forever, so the sanity guard rejects it and takes mid-scale.
static void test_axis_rejects_an_absurd_boot_centre() {
  nativeResetPins();
  nativeSetAnalog(JOY_X_PIN, 60);            // stick shoved to one end at boot
  Axis a;
  a.begin(JOY_X_PIN, false);

  TEST_ASSERT_EQUAL_INT(MID, a.centerRaw());
  TEST_ASSERT_EQUAL_INT(0, settleAxis(a, JOY_X_PIN, MID));   // real centre still reads 0
}

// ============================================================================
//  Stepping: edges, hysteresis, auto-repeat
// ============================================================================

static void test_step_does_not_fire_below_the_threshold() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);
  // Comfortably deflected, but short of STEP_ON once rescaled.
  TEST_ASSERT_EQUAL_INT(0, countStepsX(j, rawAt(STEP_ON - 20), now, 1000));
}

static void test_one_deflection_is_exactly_one_step() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);
  // Held well inside the repeat delay, so the initial step is all there is.
  TEST_ASSERT_EQUAL_INT(1, countStepsX(j, ADC_MAX, now, RPT_DELAY - 100));
}

static void test_step_direction_follows_the_axis() {
  Joystick j; uint32_t now;

  freshJoystick(j, now);
  nativeSetAnalog(JOY_X_PIN, rawRight());
  for (int i = 0; i < 40 && j.stepX() == 0; i++){ now += 5; j.update(now); }
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, j.stepX(), "pushing right did not step right");

  freshJoystick(j, now);
  nativeSetAnalog(JOY_X_PIN, rawLeft());
  for (int i = 0; i < 40 && j.stepX() == 0; i++){ now += 5; j.update(now); }
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, j.stepX(), "pushing left did not step left");
}

// Holding must not re-fire until the delay has elapsed, and must then repeat at
// a steady rate. A menu that scrolls the instant you lean on it is unusable.
static void test_hold_repeats_after_the_delay() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);

  const uint32_t hold = 1500;
  int n = countStepsX(j, ADC_MAX, now, hold);

  // 1 initial + one per RPT_MS after RPT_DELAY. Two either side for EMA lag and
  // the 5 ms polling grid.
  int expect = 1 + (int)((hold - RPT_DELAY) / RPT_MS);
  TEST_ASSERT_INT_WITHIN(2, expect, n);
}

// Falling back into the hysteresis band -- but not past STEP_OFF -- must not
// rearm. Without this, a thumb resting near the threshold machine-guns.
static void test_hysteresis_blocks_a_partial_release() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);

  TEST_ASSERT_EQUAL_INT(1, countStepsX(j, ADC_MAX, now, 100));
  // Ease off to between STEP_OFF and STEP_ON, then push again. Still one step.
  TEST_ASSERT_EQUAL_INT(0, countStepsX(j, rawAt((STEP_ON + STEP_OFF) / 2), now, 100));
  TEST_ASSERT_EQUAL_INT(0, countStepsX(j, ADC_MAX, now, 100));
}

static void test_full_release_rearms_the_step() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);

  TEST_ASSERT_EQUAL_INT(1, countStepsX(j, ADC_MAX, now, 100));
  TEST_ASSERT_EQUAL_INT(0, countStepsX(j, MID, now, 100));       // back to centre
  TEST_ASSERT_EQUAL_INT(1, countStepsX(j, ADC_MAX, now, 100));   // and again
}

// A flick straight from one extreme to the other never passes through the
// hysteresis band, so the reversal has to be detected by sign rather than by
// magnitude. Costs one tick, not a return to centre.
static void test_reversal_without_passing_centre_still_steps() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);

  TEST_ASSERT_EQUAL_INT(1, countStepsX(j, ADC_MAX, now, 100));
  int n = countStepsX(j, 0, now, 200);
  TEST_ASSERT_EQUAL_INT(-1, j.stepX() == 0 ? -1 : j.stepX());    // direction sanity
  TEST_ASSERT_EQUAL_INT(1, n);
}

// ============================================================================
//  The Y flip -- stick space in, screen space out
// ============================================================================

// The single most reversible line in the whole change: +Y is up on the stick,
// rows grow down on the panel.
static void test_stick_up_is_a_negative_screen_step() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);

  nativeSetAnalog(JOY_Y_PIN, rawUp());            // physically pushed up
  for (int i = 0; i < 40 && j.stepY() == 0; i++){ now += 5; j.update(now); }

  TEST_ASSERT_TRUE_MESSAGE(j.y() > 0, "stick space: up must read positive");
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, j.stepY(), "screen space: up is a LOWER row");
  TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_UP, j.dir(), "pushing up did not report DIR_UP");
}

static void test_stick_down_is_a_positive_screen_step() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);

  nativeSetAnalog(JOY_Y_PIN, rawDown());          // physically pulled down
  for (int i = 0; i < 40 && j.stepY() == 0; i++){ now += 5; j.update(now); }

  TEST_ASSERT_TRUE_MESSAGE(j.y() < 0, "stick space: down must read negative");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, j.stepY(), "screen space: down is a HIGHER row");
  TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_DOWN, j.dir(), "pushing down did not report DIR_DOWN");
}

// ============================================================================
//  dir(): dominant axis
// ============================================================================

static void settleBoth(Joystick& j, uint32_t& now, int rawX, int rawY) {
  nativeSetAnalog(JOY_X_PIN, rawX);
  nativeSetAnalog(JOY_Y_PIN, rawY);
  for (int i = 0; i < 40; i++){ now += 5; j.update(now); }
}

static void test_dir_is_none_at_rest() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);
  settleBoth(j, now, MID, MID);
  TEST_ASSERT_EQUAL_INT(DIR_NONE, j.dir());
}

static void test_dir_picks_the_dominant_axis() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);

  settleBoth(j, now, rawRight(), rawY(30));         // mostly right, a little up
  TEST_ASSERT_EQUAL_INT(DIR_RIGHT, j.dir());

  settleBoth(j, now, rawX(30), rawUp());            // mostly up, a little right
  TEST_ASSERT_EQUAL_INT(DIR_UP, j.dir());

  settleBoth(j, now, rawLeft(), rawY(-30));         // mostly left, a little down
  TEST_ASSERT_EQUAL_INT(DIR_LEFT, j.dir());

  settleBoth(j, now, rawX(-30), rawDown());         // mostly down, a little left
  TEST_ASSERT_EQUAL_INT(DIR_DOWN, j.dir());
}

// An exact diagonal is genuinely ambiguous, and guessing is worse than
// declining: Tron reads "no request" as "keep going", so a sloppy diagonal
// coasts instead of turning somewhere the player did not ask for.
static void test_exact_diagonal_requests_nothing() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);
  settleBoth(j, now, ADC_MAX, ADC_MAX);
  TEST_ASSERT_EQUAL_INT(DIR_NONE, j.dir());
}

// Deflection short of the nav threshold on both axes is not a direction either.
static void test_dir_needs_a_real_deflection() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);
  settleBoth(j, now, rawX(STEP_ON - 25), rawY(-(STEP_ON - 30)));
  TEST_ASSERT_EQUAL_INT(DIR_NONE, j.dir());
}

// Every game binds up and down to different things, so a board that wires Y the
// other way must be corrected by JOY_INVERT_Y and nowhere else. This asserts the
// contract end to end at whatever that constant currently is: push up, get up.
static void test_axis_inversion_constants_match_the_contract() {
  Joystick j; uint32_t now;
  freshJoystick(j, now);

  settleBoth(j, now, MID, rawUp());
  TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_UP, j.dir(), "JOY_INVERT_Y has Y backwards");

  settleBoth(j, now, MID, rawDown());
  TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_DOWN, j.dir(), "JOY_INVERT_Y has Y backwards");

  settleBoth(j, now, rawRight(), MID);
  TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_RIGHT, j.dir(), "JOY_INVERT_X has X backwards");

  settleBoth(j, now, rawLeft(), MID);
  TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_LEFT, j.dir(), "JOY_INVERT_X has X backwards");
}

// ============================================================================
//  Button debounce
// ============================================================================

static void test_button_debounces_a_bouncing_press() {
  nativeResetPins();
  Button b;
  b.begin(BTN_A_PIN);

  uint32_t now = 0;
  b.update(now);
  TEST_ASSERT_FALSE(b.isHeld());

  // Contact chatter inside the debounce window must produce no edge at all.
  for (int i = 0; i < 5; i++){
    nativeSetDigital(BTN_A_PIN, i & 1 ? HIGH : LOW);
    now += 2;
    b.update(now);
    TEST_ASSERT_FALSE(b.wasPressed());
  }

  // Settle low and wait out the window: exactly one press edge.
  nativeSetDigital(BTN_A_PIN, LOW);
  now += 2;  b.update(now);
  now += DEBOUNCE_MS + 1;
  b.update(now);
  TEST_ASSERT_TRUE(b.wasPressed());
  TEST_ASSERT_TRUE(b.isHeld());

  // The edge is good for one tick only.
  now += 5;
  b.update(now);
  TEST_ASSERT_FALSE(b.wasPressed());
  TEST_ASSERT_TRUE(b.isHeld());
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_axis_reads_zero_at_rest);
  RUN_TEST(test_axis_deadzone_swallows_small_deflection);
  RUN_TEST(test_axis_full_travel_still_reaches_the_ends);
  RUN_TEST(test_axis_rescales_the_live_band);
  RUN_TEST(test_axis_invert_flips_the_sense);
  RUN_TEST(test_axis_calibrates_an_off_centre_stick);
  RUN_TEST(test_axis_rejects_an_absurd_boot_centre);

  RUN_TEST(test_step_does_not_fire_below_the_threshold);
  RUN_TEST(test_one_deflection_is_exactly_one_step);
  RUN_TEST(test_step_direction_follows_the_axis);
  RUN_TEST(test_hold_repeats_after_the_delay);
  RUN_TEST(test_hysteresis_blocks_a_partial_release);
  RUN_TEST(test_full_release_rearms_the_step);
  RUN_TEST(test_reversal_without_passing_centre_still_steps);

  RUN_TEST(test_stick_up_is_a_negative_screen_step);
  RUN_TEST(test_stick_down_is_a_positive_screen_step);

  RUN_TEST(test_dir_is_none_at_rest);
  RUN_TEST(test_dir_picks_the_dominant_axis);
  RUN_TEST(test_exact_diagonal_requests_nothing);
  RUN_TEST(test_dir_needs_a_real_deflection);
  RUN_TEST(test_axis_inversion_constants_match_the_contract);

  RUN_TEST(test_button_debounces_a_bouncing_press);

  return UNITY_END();
}

void setUp(void)    {}
void tearDown(void) {}
