# Joystick + 16×16 hardware revision

The next-gen board replaces the pot slider with a two-axis analog joystick,
moves four pins, and quadruples the panel to 16×16. This is the plan for
getting the firmware there.

Multiplayer reworks are **explicitly out of scope**; where a decision has a
multiplayer consequence it is recorded under [Deferred](#deferred-multiplayer)
rather than acted on.

---

## 1. What actually has to change

The good news first, because it shapes everything below: **all four game
simulations are already resolution-agnostic and already tested at 16×16.**
`TetrisSim`, `BreakoutSim`, `Tron` and `Virus` take their dimensions from
`newGame()`/`begin()` and none of them reads `MATRIX_W`/`MATRIX_H`. `TET_MAX_W/H`,
`BRK_MAX_W`, `TRON_MAX_CELLS` and `VIRUS_MAX_CELLS` are all already sized for a
16×16 board, and `test_breakout` plays a full screen out at both resolutions on
every CI run. The panel upgrade is, as the code has been claiming, a
configuration change at the call sites.

So the work is not "make the games bigger". It is three separate things:

| | Scope |
|---|---|
| **A. Hardware config** | Pins, geometry, orientation, power. Contained in `defines.h` + `display.cpp`. |
| **B. The input model** | The slider was **absolute**; the joystick is **self-centering**. Every selector on the device reads position and maps it to an index. All of them break. This is the real work. |
| **C. Using the space** | 4× the pixels means playfield layouts and HUDs that were impossible on 64 pixels. This is the fun part, and it is per-game. |

B is the risk. A and C are volume.

---

## 2. Phase 0 — hardware bring-up

No game logic. Ends when the panel is correctly oriented and the buzzer works.

### Pins

| Function | Old | New | Notes |
|---|---|---|---|
| Display data | 7 | **6** | RMT-capable, not a strapping pin. |
| Button A | 6 | **3** | See the GPIO3 note below. |
| Button B | 5 | **4** | |
| Audio | 3 | **5** | |
| Slider | 4 | — | Gone. |
| Joystick X | — | **1** | ADC1_CH0. +X = right |
| Joystick Y | — | **2** | ADC1_CH1. +Y = up |

Three things worth knowing about this map:

- **Both joystick axes land on ADC1, which is required, not lucky.** ADC2 is
  unusable while WiFi is up on the ESP32-S3, and ESP-NOW keeps WiFi up for the
  whole session. On the S3, ADC1 is GPIO1–GPIO10, so pins 1 and 2 are correct.
  If either axis ever has to move, it must stay inside that range.
- **GPIO3 is a strapping pin** (JTAG signal source select). It does not affect
  boot mode, and `INPUT_PULLUP` is fine, so button A works — but holding A down
  through a reset changes the JTAG source. Harmless in normal use; worth a
  comment in `defines.h` so nobody rediscovers it while debugging.
- **The audio gate's external pull-up is already correct on the new board** and
  is staying as-is. That matters more than it looks: `defines.h` explains that
  neither the driven level nor the internal pull exists during reset and the ROM
  bootloader, and the gate is active low, so an unclaimed pin fails toward the
  FET being *on*. That external pull is the only cover for the boot window.
  `System::begin()` already calls `Audio::parkPin()` first, so the firmware side
  is just the `#define`.

### Geometry and the LED change

```c
#define MATRIX_W   16
#define MATRIX_H   16
#define DEAD_INDEX (-1)      // new panel; the bypass mechanism stays, unused
```

The new panel is **WS2812B (RGB), not SK6812 RGBW**, which simplifies two things
and complicates one.

Simpler: the RGBW buffer over-allocation goes away entirely. `LED_BUF_LEN` exists
only because RGBW is 4 bytes per pixel while `CRGB` is 3, so the buffer was
padded by 4/3. Delete it — the buffer is exactly `NUM_LEDS_DATA` `CRGB` (256,
768 bytes). And `Display::begin()` drops the RGBW conversion:

```c
FastLED.addLeds<WS2812B, DISP_LED_PIN, GRB>(_leds, NUM_LEDS_DATA);
```

no `.setRgbw(RgbwDefault())`. The RMT5 driver path is unchanged, so
`platformio.ini`'s note about avoiding the Adafruit NeoPixel RMT regression
still holds.

Complicated: **white is now synthesized from all three channels.** On the old
panel `CRGB::White` was routed to the dedicated W die and cost roughly one
channel; now it costs three. Nothing needs rewriting — white is still white —
but it changes the power arithmetic below in a way that is worth being precise
about, because it inverts which effects are expensive.

Two colour items to settle on hardware:

- **Enable `FastLED.setCorrection(TypicalLEDStrip)`.** Without a dedicated white
  die, neutral greys and whites come out tinted unless corrected. This was
  largely masked before.
- **Check the dim indicator colours survive.** The UI leans on values like
  `CRGB(18,18,18)`, `CRGB(20,20,20)`, `CRGB(0,40,40)` for "unselected" states.
  At `DISPLAY_BRIGHTNESS 8` those scale to well under one count and depend
  entirely on FastLED's temporal dithering to appear at all. Dithering needs a
  steady `show()` cadence to work, which we have — but verify them on the panel
  and raise the values if they read as simply dark.

Confirmed as a single continuous 16×16 module, so `cellToData` needs no tile
term and stays as written. But **re-run `CALIBRATION=1` and re-derive
`TRANSPOSE` / `FLIP_X` / `FLIP_Y` / `MATRIX_SERPENTINE` from scratch.** The
current values were measured off the old panel and there is no reason to expect
a different module to be wired the same way. `Display::calibrate()` already
draws exactly the pattern that answers this (green row 0, red col 0, white
origin).

### Power — the tight constraint

The 5 V rail is under 1 A, and 256 WS2812B is a materially different load from
63. Per-pixel: ~20 mA per channel at full, ~60 mA all three. At
`DISPLAY_BRIGHTNESS 8` (~3.1 % duty), against 256 pixels:

| | Draw |
|---|---|
| Quiescent, every LED off (256 × ~0.8 mA of driver IC) | **~205 mA** |
| Full screen, one channel — `fill(CRGB::Red)` | ~161 mA |
| Full screen, saturated rainbow — the splash wipe | ~240 mA |
| Full screen, **white** | **~483 mA** |
| ESP-NOW TX burst | 350–500 mA |

**The switch to WS2812B inverts the earlier conclusion here**, and it is worth
being explicit about since it changes what needs fixing. On RGBW, white came off
the dedicated W die and was cheap while coloured fills were not; now white costs
three channels and single-channel fills are the affordable ones. So:

- **The red full-screen fills are fine and can stay.** Tetris game-over,
  Breakout life-lost and Breakout game-over are all single-channel: ~161 mA,
  and with quiescent and a radio peak that is ~816 mA. Tight, but inside the
  rail. No rework needed.
- **A full-screen white would not be.** 205 + 483 + 450 ≈ 1.14 A, over. Nothing
  in the tree does this today — the white usages are all borders (60 px), the
  Tron heads (≤ 5 px), selector dots and the line-clear animation (≤ 32 px), all
  trivial. It is a rule for new work, not a repair.
- **The splash is now the device's peak-current moment, and it happens at boot.**
  `wipe()` fills all 256 pixels with fully saturated `CHSV(h,255,255)` while the
  radio is already up (`net.begin()` runs before `Splash::play()`): ~205 + 240 +
  radio ≈ 895 mA, on top of power-on inrush. Nothing else on the device gets
  close. Test the splash first on real hardware, and if the rail sags, restrict
  the wipe to a moving band of columns rather than a cumulative fill.

Three actions:

1. **`FastLED.setMaxPowerInVoltsAndMilliamps(5, 350)` in `Display::begin()`.**
   FastLED scales brightness per frame to stay inside it, so an over-budget
   frame dims instead of browning out the regulator. A brownout mid-ESP-NOW is a
   reset, not a flicker — this is a safety net, not a nicety.
2. **Re-tune `DISPLAY_BRIGHTNESS` on hardware.** 8 was chosen for 63 LEDs.
3. Budget new effects in **lit channels**, not lit pixels — that is the unit
   that actually costs current now.

### Frame time — measure this early

WS2812B is 24 bits per pixel rather than RGBW's 32, which takes some of the
sting out of 4× the pixel count: 24 bits × 1.25 µs × 256 ≈ **7.7 ms per
`show()`**, up from ~2.5 ms. Three times the frame cost, not four. With
`loop()`'s `delay(5)` that puts the practical ceiling near 78 fps. Breakout's
62 Hz fixed step with `MAX_CATCHUP 4` should hold with a little room, but not
much.

Measure it on hardware in Phase 0, before any game work. If Breakout is short,
**the fix is to skip `show()` when nothing changed, not to raise
`MAX_CATCHUP`** — raising the catch-up cap is how the ball starts tunnelling
through bricks, which is exactly what `breakout_sim.h` warns about.

---

## 3. Phase 1 — the input layer

### The shape of the problem

Every selector on the device does some variant of this:

```c
int pct  = _sys.slider.read();                  // -100..+100, absolute
int page = constrain((pct + 100) * N / 201, 0, N - 1);
```

Position maps to index. That is correct for a pot you park and wrong for a stick
that springs back: a self-centering control reading `0` at rest pins every one
of these to its middle entry, permanently. Nine call sites:

`Menu::service` · `Settings::serviceSub` / `serviceColor` / `serviceVolume` ·
`Tetris::updateMenu` · `Breakout::updateMenu` · `TronApp::serviceSub` ·
`Multiplayer::serviceBrowse` · `VirusApp::serviceSetup`

### `Joystick`, replacing `Slider`

Two reading modes, because menus and games want genuinely different things:

```c
class Axis {                      // one analog axis, the Slider math preserved
  void begin(uint8_t pin, bool invert, int deadzonePct);
  void update();
  int  read() const;              // -100..+100, deadzoned and rescaled
  int  centerRaw() const;         // sampled at boot -- see below
};

class Joystick {
public:
  void begin();
  void update(uint32_t now);

  // Proportional -- for games that want pressure.
  int    x() const;               // -100..+100
  int    y() const;

  // Edge-stepped -- for menus. One event per deflection, then auto-repeat.
  int8_t stepX() const;           // -1 / 0 / +1, valid this tick only
  int8_t stepY() const;

  // Held four-way, dominant axis, DIR_NONE at rest.
  Dir    dir() const;
};
```

The deadzone rescale from `Slider::update` carries over unchanged — the comment
explaining why it rescales rather than flattens is still exactly right, and
Breakout still cares for the same reason.

Tunables (all to be confirmed on hardware):

| | |
|---|---|
| `JOY_DEADZONE 12` | Larger than the slider's 6. A spring-return stick's rest point wanders more than a pot you set down. |
| `JOY_STEP_ON 55` | Deflection % at which a nav step fires. |
| `JOY_STEP_OFF 35` | Must fall back below this to rearm. Hysteresis, so resting near the threshold doesn't chatter. |
| `JOY_REPEAT_DELAY_MS 320` | Hold before auto-repeat starts. |
| `JOY_REPEAT_MS 110` | Repeat period. |

**+Y is up, and the screen's rows grow downward.** Worth stating once, loudly,
because it is a sign flip that gets reversed exactly one time by exactly one
person. `setPixel(col, row)` puts row 0 at the top, and every game already
treats y as growing down (`Tron`'s `DY[4] = {0,1,0,-1}`, `virus.cpp`'s "y grows
downward"). So `joy.y() > 0` — stick pushed up — must produce a *decreasing*
row.

Keep `Joystick::y()` in stick space, where + is up, because that is what a human
reasons about and what `JOY_INVERT_Y` is calibrated against. Then convert in one
place: **games consume `dir()` and `stepY()`, never the raw sign of `y()`.** The
`Dir` enum carries `DIR_UP`/`DIR_DOWN` with no ambiguity, and `stepY()` is
defined as a *screen* step (+1 = down) with the flip applied inside `Joystick`.
One conversion, one place, testable in `test_controls`.

**Boot-time center calibration.** `Axis::begin()` samples the resting ADC and
uses it as center rather than assuming `ADC_MAX/2`. Thumbsticks rarely rest at
exactly mid-scale, and a fixed center means one direction has more usable travel
than the other — which shows up as a paddle that drifts on its own. Guard it:
accept the sampled center only if it is within ±20 % of mid-scale, otherwise
fall back to mid-scale. That covers the case where the stick is held at boot.

### No compatibility shim

It is tempting to keep `sys.slider.read()` as an alias for `joy.x()` so the tree
keeps building while it is ported. **Don't.** That shim compiles cleanly and
leaves every menu on the device silently stuck on its middle entry — the worst
available failure mode, because nothing announces it. Break the build instead;
nine call sites is an afternoon, and a compiler error names each one.

### New test coverage

Add `src/core/controls.cpp` to the native env's `build_src_filter` and write
`test/test_controls/`. The shims already supply `analogRead` and `millis`, and
the deadzone rescale, hysteresis, step edges, repeat timing and the center-sanity
guard are all pure arithmetic over a fake ADC. This is precisely the class of
bug that is miserable to chase on hardware — a stick that double-steps once in
twenty is nearly untestable by hand and trivial to pin here.

---

## 4. Phase 2 — the shell

Ends with a fully navigable device. Games still play with whatever input port
they have at that point.

### The nine selectors

Each becomes relative:

```c
int8_t s = _sys.joy.stepX();
if (s){ _sel = wrap(_sel + s, N); _sys.audio.play(SFX_MOVE); }
```

Three things fall out of this for free:

- **Wrap-around**, which a slider could never do.
- The blip logic simplifies. Every one of these currently carries a comment
  explaining that it blips on the *change* rather than the position, because the
  slider is re-read every frame. The step *is* the change; the comments go away
  with the problem.
- `Menu` keeping `_page` across game entry ("returning from a game lands where
  you left") stops being a slider side-effect and becomes the actual design.

One subtlety: **`Multiplayer::serviceBrowse`'s option count moves underneath the
selection** as lobby beacons arrive and go stale. It currently re-derives from
position every frame so this is self-correcting; with relative selection, clamp
`_sel` when the count shrinks.

### Main menu presentation

`drawBitmap` already scales the existing 8×8 icons by 2× and centers them, which
fills a 16×16 panel exactly — so the icons carry over untouched, as intended.

What is lost is positional feedback: a slider told you where you were in the
list by where your thumb was. Replacing it:

- **Slide transition.** On each step, the outgoing icon slides off and the
  incoming one slides on over ~120 ms. The motion carries the direction.
- **Fading position strip.** A row of N pixels, current one white, drawn over
  the bottom row and **faded out ~500 ms after the last step** rather than living
  there permanently. Same reasoning as Breakout's lives display: show it when it
  is the thing the player wants to know, not always.

The slide needs a second entry point — `drawBitmapAt(img, pal, scale, x0, y0)`,
with the existing `drawBitmap` becoming the centered wrapper. `display.h`'s
comment already anticipates exactly this ("should add a second entry point
rather than change this").

Up/down on the main menu stays **unused**. There is no second dimension to the
game list and inventing one would only make the list feel like it has a secret.

### Settings gets the second axis

This is where up/down earns its keep. Left/right selects the page, **up/down
changes the value in place**, A saves, B backs out. That collapses `SUB` /
`COLOR` / `VOLUME` into a single state and removes an enter/exit step.

Volume gets better as a side effect: the preview-on-change behaviour is
preserved, and you no longer have to enter a sub-page before you can hear what
you are setting. Keep the cancel-restores-previous semantics — previewing stays
free.

16 columns is also enough for a real label next to each control, which the 8×8
never had room for.

---

## 5. Phase 3 — the games

One branch each. Recommended order and why:

1. **Breakout** — smallest change, and it proves the analog path end to end.
2. **Tetris** — largest UI change; benefits from the HUD helpers Breakout adds.
3. **Tron** — changes the wire format, so it wants a stable base under it.
4. **Virus** — tuning plus a HUD; least risky, lands last.

### Breakout

**Playfield.** `brickRowsFor(16)` → 4 rows, `paddleWidthFor(16)` → 6 wide,
`baseSpeed()` already scales with height. All correct with no code change.

Retuning: 4 rows × 16 = **64 bricks against today's 16**, so a screen takes four
times as long to clear. `SPEEDUP_HITS_1 4` and `SPEEDUP_HITS_2 12` are Atari's
schedule expressed as absolute brick counts, and they now fire in the first few
seconds of a much longer screen. Derive them from the brick count instead — a
quarter and a half of the screen — so both resolutions behave the same.

**Paddle control — the important decision.** Porting the slider straight across
(absolute position) is wrong: release the stick mid-rally and the paddle snaps to
center. Use **velocity control** instead — paddle position accumulates at a rate
proportional to deflection:

```c
void BreakoutSim::steerPaddle(int pct);   // once per tick; integrates into _padPos (Q8)
```

Full deflection traverses the 16-wide board in ~0.8 s; a light push nudges it a
fraction of a cell per tick. **This is the single best use of proportional
pressure on the device** — Breakout is the one game where sub-cell positioning
and fast traverse are both needed constantly, and an analog stick gives you both
from one gesture where a digital pad gives you neither.

`setPaddle(pct)` becomes test-only `placePaddle(col)`, which keeps
`test_breakout`'s dual-resolution coverage intact with a one-line change per
call site.

**HUD.** Bricks occupy rows 0–3, so row 5 is permanently free. Drop the
"only show lives right after losing one" workaround — that was a 64-pixel
compromise, and the comment says so. Permanent lives pips at row 5.

Up = launch, as an alternative to A. A stays primary.

### Tetris

**Layout**, per the proposed split:

```
cols 0-7    playfield, 16 tall          -> _sim.newGame(8, 16, seed)
col  8      dim divider
cols 9-15   info panel, 7 wide
```

An 8×16 well is the real Tetris aspect ratio, which the 8×8 board never was.

Info panel (7 columns is tight, and this is what honestly fits):

| Rows | Content |
|---|---|
| 1–4 | Next piece, 4×4 at cols 10–13 |
| 6–10 | Level, two 3×5 digits at cols 9–15 (3 + 1 + 3 = 7, exactly) |
| 13 | Lines-to-next-level, a 7-wide progress bar |

Score stays on the game-over scroll. Five digits is 19 px in the 3×5 font and
there is no honest way to fit it in seven columns without a marquee, and a
permanently moving element beside the well is a distraction that also costs a
redraw every frame.

**Controls:**

| Input | Action |
|---|---|
| Left / right | Move, with DAS |
| Down | Soft drop |
| Up | **Hard drop** (new) |
| A | Rotate CW |
| B | **Rotate CCW** (new — B is free now that soft drop moved to the stick) |

**Recommendation: digital DAS, no pressure modulation, for Tetris specifically.**
The sim owns it — `setMove(int8_t dir)` replacing `setSlider(int pct)`, with
`DAS_DELAY_MS 170` and `ARR_MS 50` replacing the current uniform 55 ms move gate.
Tap moves one cell immediately; hold waits DAS, then repeats at ARR.

The reason not to modulate the repeat rate by deflection here — even though the
hardware supports it — is that Tetris placement is muscle memory. A player
learns "two taps and a hold gets me to the left wall". A repeat rate that varies
with how hard they happen to be pushing makes every placement a fresh
negotiation. Save pressure for the games where the quantity being controlled is
itself continuous.

Hard drop is edge-triggered only: the stick must return to center before another
fires. `TetrisSim::hardDrop()` drops to `ghostY()`, locks, and scores. Reuse
`TET_EV_LOCK` for the event and give `tetris.cpp` a distinct sound for the
hard-drop case — a thud with more weight than a soft lock.

`rotate()` gains a direction argument.

### Tron

**Absolute heading**, as requested. Currently `steer` is relative (−1/0/+1
applied on each step while held), which was the only thing a slider could
express. Direct heading is strictly better with a four-way stick.

**Wire format stays two bytes**, so nothing in the protocol resizes:

```
buf[0] = desired heading 0..3, or 0xFF for "no request"
buf[1] = boost
```

- `applyInput` stores the request; `hostTick` adopts it unless it is the reverse
  of the current heading (`(want ^ 2) == dir`). **Reversal must be rejected in
  the sim, not the UI** — a reversal is instant self-collision, and the client is
  untrusted.
- `Player::steer` goes away.
- **The AI gets simpler.** `aiInput` already computes `cand[bestK]`, which *is*
  an absolute heading; it currently converts that back to a relative turn. Emit
  it directly and delete the conversion.
- **Diagonals resolve statelessly** in `serializeInput`: dominant axis wins on a
  strict `>`, an exact tie emits `0xFF` (no request). Stateless on purpose — a
  `NetGame` should not carry hidden input history, and "no request" already means
  "keep going", so a tie costs nothing.

**Retuning for 4× the area.** All three of these want playtesting, and all three
should become *derived from arena area* rather than constants, so 8×8 units
still behave:

| | Now | 16×16 | Why |
|---|---|---|---|
| `BASE_STEP_TICKS` | 20 (1.25 cells/s) | 10 (2.5 cells/s) | 12.8 s to cross a 16-wide arena is not a game. |
| `BOOST_STEP_TICKS` | 10 | 5 | Keep the 2× ratio. |
| `TRON_START_LEN` | 3 | `max(3, cells/32)` = 8 | A 3-long trail in 256 cells is not an obstacle. |
| `TRON_GROW_TICKS` | 150 | ~80 | 4× the ground to fill; matches should stay the same length. |

**Payload watch item, not a blocker.** `serializeState` at 16×16 is
3 + 5×3 + 128 = **146 bytes**, up from 35, broadcast every `MATCH_TICK_MS`
(40 ms). Well inside `NET_STATE_MAX` (240), but 4× the air time. If it ever
bites, the owner map is highly compressible — note it, don't build it.

B stays free. A brake would change the game's character and deserves to be its
own decision rather than a rider on this one.

### Virus

**Board 16×14, HUD rows 14–15.** `begin(MATRIX_W, MATRIX_H - 2, ...)` — the
referee is entirely `arenaW`/`arenaH`-driven, so a non-square arena is a
call-site change. 224 cells is inside `VIRUS_MAX_CELLS` (256), and
`serializeState` comes to 5 + 56×3 = **173 bytes**, comfortable.

**Row 14 — live territory bar.** Sixteen pixels split proportionally by each
virus's current cell count, in that virus's color, updated every tick. That is
the running score, readable at a glance, for one row. It is the single most
useful thing this game could put on screen: Virus is a spectator game, and
"who is winning right now" is currently only inferable by eyeballing the board.

**Row 15 — match clock**, a bar draining left to right over
`VIRUS_MATCH_TICKS`.

The bar needs one new accessor, `Virus::cellCount(uint8_t player)`. The referee
already computes per-player counts every tick for stall detection (`_cntHist`),
so this exposes something that exists rather than adding work to the tick.

**Retuning for 3.5× the cells.** Energy income, action costs and match length
were all tuned against 64 cells. The principle: **income scales with board area,
costs stay fixed.** That keeps each virus's per-cell economy — the actual game,
per `virus_rules.cpp` — unchanged, while letting a match reach the same
territory fraction in the same wall-clock time. `VIRUS_MATCH_TICKS 160` (~29 s)
likely wants ~240 (~43 s); `VIRUS_STALL_WINDOW 24` probably stands. All of this
is a measurement task, run against the four shipped viruses.

**Setup screen** at 16 wide: four blocks of 3 columns with 1-column gaps, each
labelled with its 3×5 letter A/B/C/D — which finally fits, and which
`virus-plan.md` wanted from the start. Cursor row on top, GO bar across the
bottom two rows.

No live input during a match, so the joystick touches only the setup UI.

### Splash and scoreboard

`drawScoreScroll` uses `MATRIX_W`/`MATRIX_H` and works untouched, but the 3×5
font on a 16×16 panel is now small enough to be an afterthought. Add a scaled
number draw and run the score marquee at 2×. `drawWinBars` already spaces itself
across `MATRIX_W` and gets better for free — 16 rows of bar resolution instead
of 8.

`Splash` needs two fixes. `TITLE_ROW 1` is hardcoded for "5-tall font
centered-ish on the 8-tall panel" and wants `(MATRIX_H - 5) / 2` — the title
currently renders near the top edge of a 16-tall panel. And per §2, the rainbow
wipe is the highest-current frame the device ever draws, at the worst possible
moment; if the rail sags at boot, convert it from a cumulative fill to a moving
band of lit columns. Both are small, but the splash is the first thing anyone
sees on the new hardware, so it should land in Phase 0 alongside calibration
rather than waiting.

---

## 6. Phase 4 — tests and docs

**Tests that must change:**

- `test_tetris` — `setSlider(SLIDER_LEFT/RIGHT)` in seven places becomes
  `setMove(-1/+1)`. Add DAS/ARR assertions (a tap moves exactly one cell; a hold
  moves one, pauses, then repeats at a steady rate) and hard-drop tests.
- `test_breakout` — paddle placement switches to `placePaddle()`. Add velocity
  integration tests. The existing dual-resolution suite is the thing that makes
  this whole upgrade cheap; keep both resolutions running.
- `test_tron` — the two input bytes change meaning. Add a **reversal-rejection**
  test: request the opposite heading, assert the player continues and does not
  die.
- `test_controls` — new, per Phase 1.

**Docs:**

- `README.md` — hardware table (pins, panel, controls), the intro line that says
  "two buttons and a slider", and the quit-gesture paragraph if B's role changes.
- `docs/architecture.md` — the controls line (§48), Tron's control description
  (§206) and the phase-3 verification steps (§251) all describe slider steering.
- This document moves into the design-record set in `docs/plans/README.md` once
  the work ships.

---

## 7. Deferred: multiplayer

Recorded, not acted on.

- **`LocalInput` gains a second axis.** `int16_t slider` becomes `int16_t x, y`.
  No protocol size change; `NET_INPUT_MAX` is untouched.
- **Bump `NET_PROTO_VER` to 4.** Tron's input byte 0 changes meaning (relative
  steer → absolute heading) at the same size, so a mixed fleet would mis-decode
  *silently*. The version byte is what turns that into a loud failure. Note that
  `LobbyPayload` is already self-describing about arena size, so a 16×16 unit and
  an 8×8 unit already refuse each other correctly — that half is handled.
- **Networked Virus** at 224 cells is 173 B per state, the heaviest payload on
  the device — but its tick is 180 ms, so it broadcasts at ~5.5 Hz. Comfortable.
- **Head-to-head Tetris** becomes natural on this hardware: an 8-wide well per
  unit is the standard shape, and garbage lines are the obvious mechanic.
- **Tron at 16×16 with 5 players** is roomy where 8×8 was cramped. The spawn
  table (`sx`/`sy` in `Tron::begin`) already scales and needs no change, but the
  match will feel different enough to want its own tuning pass.

---

## 8. What actually shipped, and where it differed

Four things came out differently once the code was written. Recorded because the
reasoning changed, not just the outcome.

- **The power conclusion inverted, and §2 was rewritten around it.** The first
  draft of this plan was written against SK6812 and concluded the full-screen
  red fills had to go. WS2812B reverses that: white costs three channels instead
  of one, single-channel fills cost less than estimated. The red flashes stayed;
  the rule became "no full-screen white", which nothing was doing anyway.
- **`NET_PROTO_VER` was bumped to 4 now rather than deferred to the multiplayer
  work.** Tron's input byte changed meaning in this change, not the next one, so
  leaving the version alone would have left a real silent-mismatch window open
  in between.
- **`Axis::update` rounds instead of truncating.** `test_controls` caught full
  deflection reporting 98 rather than 100 — the EMA settles one count shy of its
  target, that reads as 99, and the deadzone rescale turns 99 into 98. Invisible
  in a menu; it would have made the paddle's top speed permanently unreachable.
  Exactly the class of bug that suite was added for, found on its first run.
- **`enum Dir` had to become `JoyDir`.** The Virus authoring API already owns
  `Dir` (`Dir::N`/`E`/`S`/`W`), and that one is edited by virus authors in
  `virus_rules.cpp`, so the new name gave way rather than the published one.

Everything else landed as written. Both firmware envs build; all 91 native tests
pass, including the four new suites' worth of coverage described in §6.

## 9. Measurements to take on first hardware

Everything here is settled except what only hardware can tell us. Four numbers,
all taken before any game work starts, because Phase 3's tuning depends on them:

1. ~~**Panel orientation.**~~ **Done.** `CALIBRATION=2` showed one white marker
   per row alternating sides down the whole panel, row 0's marker at the right,
   and hue running top to bottom: a continuous 16-wide **serpentine** raster
   entering at the **top-right**. Settled as `MATRIX_SERPENTINE true`,
   `FLIP_X true`, `FLIP_Y false`, `TRANSPOSE false`, untiled.

   Worth recording *how* this surfaced, because it is the trap this section
   exists for: it was reported as a Breakout bug — the ball looked confined to
   an 8-wide playfield. It was not. An undeclared serpentine leaves full rows of
   bricks looking perfectly correct, because a mirrored full row is still a full
   row; only the moving objects give it away, and they read as bad game logic
   rather than as bad wiring. **Calibrate before judging any game.**
2. **Stick centre and travel.** Resting ADC on both axes, and the raw value at
   each extreme. Sets the deadzone, confirms `JOY_INVERT_X/Y` (+X right, +Y up),
   and validates the ±20 % boot-centre sanity guard.
3. **`show()` duration and Breakout's catch-up margin**, against the ~7.7 ms
   estimate in §2. If the margin is thin, the fix is skipping `show()` on
   unchanged frames.
4. **Actual rail current** at working brightness with the radio transmitting —
   specifically during the splash wipe, which §2 identifies as the peak. Set the
   FastLED power cap from that measurement rather than from the estimate.
