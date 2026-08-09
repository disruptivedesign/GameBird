#pragma once
// ============================================================================
//  Board-specific hardware configuration (pins, geometry, panel quirks).
//  Game-agnostic; every module includes this for the physical layout.
// ============================================================================

// ---- Pins ----
// Rev 2 board: the slider is gone, replaced by a two-axis analog joystick, and
// four pins moved. Nothing here is interchangeable with a rev 1 unit -- the old
// display pin is now button A, and the old audio pin is now button B's
// neighbour -- so a rev 1 binary on a rev 2 board drives the buzzer gate from
// what is now an input. Flash the whole fleet together.
//
// Second pass on rev 2: button A and B moved again (were 3/4) to free GPIO3 for
// a joystick button and GPIO4 for a dedicated 3.3V supply to the joystick pots.
// Same warning applies -- flash the whole fleet together.
#define DISP_LED_PIN   6    // WS2812B matrix data line
#define BTN_A_PIN      13   // button A - active low, pullup
#define BTN_B_PIN      12   // button B - active low, pullup
#define AUDIO_PIN      5    // passive piezo through a FET, driven by LEDC PWM
#define JOY_X_PIN      2    // joystick X, ADC1_CH1  (swapped -- see axis note below)
#define JOY_Y_PIN      1    // joystick Y, ADC1_CH0  (swapped -- see axis note below)
#define JOY_BTN_PIN    3    // joystick button - active low, pullup (see GPIO3 note).
                             // Tetris's counter-clockwise rotate; unused elsewhere.
#define JOY_PWR_PIN    4    // joystick potentiometer 3.3V supply - output, driven
                             // HIGH at boot and held there for the life of the program

// Both joystick axes MUST stay on ADC1 (GPIO1..GPIO10 on the S3). ADC2 is
// unusable while WiFi is up, and ESP-NOW keeps WiFi up for the whole session --
// so an axis on ADC2 reads garbage exactly when a match is running.
//
// GPIO3 is a strapping pin (JTAG signal source select). It does not affect boot
// mode and INPUT_PULLUP is fine, so the joystick button works normally; but
// holding it down through a reset changes which JTAG source the chip listens
// to. Harmless in use, confusing if you are mid-debug and wondering why the
// probe went away.

// ---- Matrix geometry ----
#define MATRIX_W   16
#define MATRIX_H   16
#define NUM_CELLS  (MATRIX_W * MATRIX_H)

// ---- Dead-pixel bypass ------------------------------------------------------
// Rev 1's panel had one LED bridged out (DIN->DOUT), which shortened the chain
// and shifted every position past the hole. The rev 2 panel is whole, so this is
// -1 and cellToData's remap compiles away. The mechanism stays: it cost little
// and the next panel with a dead pixel will want it.
#define DEAD_INDEX     (-1)
#define NUM_LEDS_DATA  (NUM_CELLS - (DEAD_INDEX >= 0 ? 1 : 0))

// ---- Panel orientation ------------------------------------------------------
// MEASURED off the rev 2 panel with CALIBRATION 2, which paints the chain in raw
// data order. What it showed:
//
//   - one white marker per row, ALTERNATING sides all the way down (row 0 right,
//     row 1 left, row 2 right, ...) -> serpentine, and exactly 16 LEDs per row,
//     so the panel is one continuous raster and not four modules
//   - the marker on row 0 is at the RIGHT, so the data line enters at the
//     top-right and even rows run right-to-left
//   - hue ramping red at the top through to red again at the bottom, so rows
//     advance downward
//
// cellToData's serpentine reverses ODD rows, and this panel reverses even ones,
// which is what FLIP_X is doing here -- it shifts the parity rather than
// mirroring anything the player sees. FLIP_Y stays false because row 0 really is
// the top row.
//
// Symptom guide if a future panel differs, because these fail in ways that look
// like game bugs rather than like display bugs:
//   - Everything mirrored or rotated      -> FLIP_X / FLIP_Y / TRANSPOSE
//   - Alternate ROWS mirrored. Full rows of bricks still look right (a mirrored
//     full row is still a full row), but a moving ball appears to jump sideways
//     as it crosses rows, and the paddle answers the stick backwards
//                                         -> MATRIX_SERPENTINE
//   - The picture is sliced into strips, each the width of one module, stacked
//     down the panel. A 16-wide playfield reads as 8 wide
//                                         -> PANEL_TILE_W / PANEL_TILE_H
#define MATRIX_SERPENTINE  true
#define FLIP_X             true
#define FLIP_Y             false
#define TRANSPOSE          false

// ---- Panel tiling -----------------------------------------------------------
// A "16x16 panel" is often four 8x8 modules chained module by module rather than
// one continuous raster: the data line fills all 64 pixels of the first module
// before reaching the second. Set these to the MODULE size when that is how
// yours is built.
//
// THIS panel is continuous -- the chain map put exactly one marker per row, so
// rows really are 16 consecutive LEDs -- which makes these the panel size and
// the identity case: cellToData reduces to exactly the expression it used before
// tiling existed, so this costs nothing here. Kept because the next panel may
// not be, and the symptom is very hard to read as a wiring problem.
#define PANEL_TILE_W  MATRIX_W
#define PANEL_TILE_H  MATRIX_H

// ---- Display ----
// 8 flickered on dim UI colours (the Tetris panel divider, its progress-bar
// off segments): FastLED temporally dithers whenever brightness < 255 to fake
// extra bit depth, alternating between two output levels frame to frame, and
// that alternation is the flicker at levels this low. Raised a bit so fewer
// dither steps are needed; dithering itself is also off, see Display::begin.
#define DISPLAY_BRIGHTNESS 14

// WS2812B decodes each bit by pulse width, not by a clock edge, so a data
// line marginal against noise doesn't corrupt whole frames -- it drops the
// occasional bit, which looks like a single pixel flickering at random on an
// otherwise-correct panel. Two independent margins against that:
//
//   1. Drive strength on DISP_LED_PIN is set to GPIO_DRIVE_CAP_3 (strongest)
//      in Display::begin(), for sharper edges than the ~medium default.
//   2. The protocol timing itself is slowed 15% below the 800 kHz nominal --
//      more like "underclocking", and well inside the tolerance FastLED
//      documents for WS2812. Set via FASTLED_OVERCLOCK_WS2812 in
//      platformio.ini rather than a #define here: that macro has to be
//      visible before FastLED.h is first preprocessed, in EVERY translation
//      unit that includes it (several do, not just this file), and a command-
//      line define is the only form of that guaranteed to win the race.
//      This does NOT touch the game's update rate: transmitting a full
//      256-LED frame is under 1 ms even at this slower rate, negligible next
//      to a game tick, so games still call show() exactly as often as before.

// Hard ceiling handed to FastLED, which scales brightness per frame to stay
// inside it. Not a nicety: the 5 V rail is under 1 A, 256 WS2812B idle at
// ~205 mA before anything lights up, and an ESP-NOW TX burst adds 350-500 mA on
// top. A frame that overruns the rail is a brownout, and a brownout mid-match is
// a reset rather than a flicker. Set this from a real measurement (see the plan
// record); the number below is the estimate it starts from.
#define DISPLAY_MAX_MA  350

// White costs three channels on WS2812B. Rev 1's SK6812 had a dedicated white
// die, so full-screen white was CHEAPER than a colour fill; here it is three
// times the price of one. Single-channel fills (the red game-over flashes) are
// ~161 mA and fine. A full-screen white would be ~483 mA and is not -- keep
// white for borders, cursors and highlights, which is all anything uses it for.

// Menu/icon art is authored at this size on EVERY panel, independent of
// MATRIX_W/MATRIX_H. Display::drawBitmap blows it up by the largest whole
// factor that fits and centers the result, so one set of bitmaps serves an 8x8
// panel 1:1 and fills a 16x16 one at 2x. A game that later wants native-
// resolution art should use drawBitmapAt rather than change this -- every icon
// in the tree depends on the 8x8 authoring size.
#define ICON_W  8
#define ICON_H  8

// ---- Controls ---------------------------------------------------------------
#define ADC_MAX          4095
#define DEBOUNCE_MS      25

// Axis sense. The logical contract is +X = right and +Y = UP; these make the
// hardware honour it. MEASURED, not assumed -- CALIBRATION 3 lights the edge the
// stick reports, so push up and see which edge answers.
//
// JOY_X_PIN and JOY_Y_PIN are swapped from GPIO number order (see the pins
// above); both axes read raw here, uninverted.
//
// +Y = up is deliberately the OPPOSITE of screen rows, which grow downward.
// Joystick::stepY() applies that flip once, on the way out, so games never do it
// themselves -- see controls.h.
#define JOY_INVERT_X     false
#define JOY_INVERT_Y     false

// Percent of travel either side of center that reads as exactly 0. The live
// band is rescaled back out to the full range (see Axis::update), so raising
// this costs resolution near center but never total travel.
//
// Larger than rev 1's slider deadzone of 6: a spring-return stick's rest point
// wanders more than a pot you physically set down, and the cost of a too-small
// deadzone here is a Breakout paddle that creeps on its own.
#define JOY_DEADZONE     12

// Nav stepping. STEP_ON fires an edge; the axis must fall back below STEP_OFF
// before it can fire again, so resting near the threshold cannot chatter.
#define JOY_STEP_ON      55
#define JOY_STEP_OFF     35
#define JOY_REPEAT_DELAY_MS  320   // hold this long before auto-repeat starts
#define JOY_REPEAT_MS        110   // then one step per this

// ---- Audio ----
// The buzzer element is passive and switched by a FET: it makes no sound on its
// own, it reproduces whatever square wave we clock at it. Three consequences
// drive everything in src/audio:
//
//   1. The gate is ACTIVE LOW -- see AUDIO_ACTIVE_LOW below. Get this one right
//      before anything else, because backwards is not a quiet-versus-noisy
//      question: it parks the FET hard on.
//   2. Loudness is DUTY CYCLE, not amplitude. 50% is the loudest a square wave
//      gets -- there is nothing above AUDIO_DUTY_HIGH, only distortion of the
//      mark/space ratio. The steps below are deliberately non-linear because
//      perceived loudness is; retune them on hardware. They are expressed as
//      FET-ON time, the thing a human reasons about, and audio.cpp complements
//      them for the gate's polarity.
//   3. Silence must be WRITTEN, at the IDLE LEVEL, never merely stopped. A
//      detached pin floats the gate, which can hold the FET part-on and sit DC
//      across the element -- a continuous buzz that also heats the FET. So the
//      pin is only a PWM output while a note is actually sounding: between notes
//      and during rests, LEDC is handed back and the pin becomes a plain output
//      driven to the idle level. The pad's internal pull is also held in the
//      safe direction permanently, covering the instants between the two.
//      This applies to the BOOT WINDOW too, the easy one to miss: the pin leaves
//      reset as a high-impedance input and stays that way until something
//      claims it, so System::begin() drives it to the idle level via
//      Audio::parkPin() before the display, NVS or the radio start up. Neither
//      the driven level nor the internal pull exists during reset and the ROM
//      bootloader -- an EXTERNAL gate pull-up is the only cover for that, and it
//      is present on the rev 2 board alongside AUDIO_PIN's move to 5.

// The buzzer board switches the element with a P-channel FET on the HIGH side,
// so pulling AUDIO_PIN LOW turns the FET ON, and the idle level is HIGH.
//
// Diagnosed on hardware rather than from the schematic: with the pin idling low
// the element buzzed continuously and the FET ran warm to the touch. Neither is
// anything a square wave does; both are what DC through the element does.
//
// Set this to 0 if the board is ever respun with an N-channel low-side switch.
// It is the only line that has to change -- everything in src/audio derives from
// it, and nothing outside src/audio touches AUDIO_PIN.
#define AUDIO_ACTIVE_LOW  1
#define AUDIO_IDLE_LEVEL  (AUDIO_ACTIVE_LOW ? 1 : 0)

// 10-bit resolution: LEDC's usable depth is log2(80 MHz / freq), so 10 bits is
// valid up to ~78 kHz -- far past anything this element reproduces.
#define AUDIO_LEDC_BITS  10
#define AUDIO_DUTY_FULL  (1 << AUDIO_LEDC_BITS)   // full scale, for complementing
#define AUDIO_DUTY_LOW   24    // ~2% FET-on
#define AUDIO_DUTY_MED   96    // ~9% FET-on
#define AUDIO_DUTY_HIGH  512   // 50% -- maximum for a square wave

// ---- Diagnostics ----
// 0 = play normally
// 1 = Display::calibrate()      -- origin + axis directions (confirms a fix)
// 2 = Display::calibrateChain() -- the raw LED chain, ignoring every setting
//                                  above (diagnoses one from scratch)
// 3 = input test                -- the edge the stick reports lights up, a dot
//                                  tracks it proportionally, buttons light the
//                                  bottom corners. Push up: if the BOTTOM edge
//                                  answers, JOY_INVERT_Y is wrong.
#define CALIBRATION  0
