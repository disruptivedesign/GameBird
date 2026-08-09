# GAMEBIRD

Handheld LED game console firmware — an ESP32-S3 driving a 16×16 RGB panel, two
buttons and a joystick. Runs Tetris, Breakout, a wireless multiplayer Tron,
**Swarm** — four units on the same side against waves of aliens, each player
carrying a different weapon — and **Virus**, a game you play by writing C++
instead of by pressing buttons.

Units talk to each other directly over ESP-NOW — no router, no pairing codes,
no office WiFi.

## Hardware

| | |
|---|---|
| MCU | ESP32-S3FH4R2 (dual core, 240 MHz, 4 MB flash, 2 MB PSRAM) |
| Display | 16×16 WS2812B matrix on pin 6 (256 px) |
| Controls | 2-axis analog joystick on pins 1/2, buttons A/B on pins 3/4 |
| Audio | Passive piezo through a FET on pin 5 |
| Radio | ESP-NOW on channel 1, up to 5 units per match |

Pins, panel orientation, the power ceiling and the joystick tuning all live in
[`src/core/defines.h`](src/core/defines.h). Both joystick axes must stay on
ADC1 (GPIO1–GPIO10): ADC2 is unusable while WiFi is up, and ESP-NOW keeps WiFi
up for the whole session.

**Calibrate the panel before judging anything on screen.** Set `CALIBRATION 2`
to paint the LED chain in raw data order — it ignores every orientation and
tiling setting, so it shows how the panel is actually wired: where the data line
enters, which way rows run, whether they alternate, and whether a "16×16 panel"
is really four 8×8 modules. Set the flags from that, then confirm with
`CALIBRATION 1`. A wrong mapping does not look like a display fault — an
undeclared serpentine makes a ball appear to jump sideways between rows while
full rows of bricks still look fine, and undeclared tiling slices the picture
into module-width strips so a 16-wide playfield reads as 8 wide.

**The joystick reads two ways.** `x()`/`y()` are proportional, for a quantity
that is itself continuous — Breakout's paddle velocity. `stepX()`/`stepY()` are
edge-stepped with hysteresis and auto-repeat, which is what every menu wants.
Rev 1's slider could do neither: a pot maps position to an index, and a stick
springs back to centre, so a ported selector would sit on its middle entry
forever. `+Y` is up on the stick and rows grow down on the panel; that flip is
applied once, inside `Joystick`, and games consume `dir()`/`stepY()` rather than
the raw sign of `y()`.

**Hold A+B for a second to leave any game** and return to the main menu. The
border brightens while the hold charges, so an accidental one is visible before
it fires; releasing early cancels. It has to be both buttons because games own
their inputs during play — Tetris rotates on both A and B — and nothing uses the
pair.

Icon art is authored 8×8 on every panel; `Display::drawBitmap` scales it up by
whole factors and centers it, so the 16×16 panel draws the existing icons at 2×
with no changes to the art. `drawBitmapAt` is the explicit-placement version the
main menu's slide transition uses.

**Watch the power budget.** 256 WS2812B idle at ~205 mA before anything is lit,
an ESP-NOW burst adds 350–500 mA, and the rail is under 1 A. White costs three
channels now that there is no dedicated white die, so a full-screen white fill
(~483 mA) is out while a single-channel fill (~161 mA) is fine. `Display::begin`
sets a hard FastLED power cap; budget new effects in lit *channels*, not pixels.

## Build and flash

Requires [PlatformIO](https://platformio.org/). Both firmware envs build with a
bare:

```bash
pio run
```

Flash a unit:

```bash
pio run -e esp32s3_supermini -t upload
```

Serial monitor is 115200 over the native USB-C port:

```bash
pio device monitor
```

## Tests

Six suites — Tron, the Virus referee, Tetris, Breakout's physics, Swarm, and the
joystick — run on a PC. The five sims are pure deterministic logic with no
Arduino calls; the joystick gets there through a fake ADC in the shims, because
a stick that double-steps one time in twenty is close to untestable by hand. No
board, no flashing, about ten seconds:

```bash
pio test -e native
```

This compiles only those sims against small Arduino/FastLED shims in
`test/shims/`. It is the fast loop for changing game rules; the firmware builds
never see the shims. CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml))
runs this and both firmware builds on every push.

Test suites live in `test/test_*/` and are picked up automatically.

## Writing a virus

**Start here: [`src/games/virus/virus_rules.cpp`](src/games/virus/virus_rules.cpp).**

Virus is a programmable cellular game. Each player is a `decide()` function
compiled into the firmware. The referee calls yours once per tick for every cell
you own, hands it that cell and the match state, and you return one action —
Grow, Move, Attack, Fortify, Spore or Idle. Every cell has its own small purse,
and your virus's income is split across every cell you own, so the more ground
you hold the poorer each cell is; deciding when a cell spends and when it saves
is the game. Own the most cells when the match ends.

The rule is pure and nearly blind: it sees its own tile, its own energy, its
four neighbours, and whether the tile three steps away is free. Everything that
looks like coordinated behaviour has to emerge from that.

The header comment in `virus_rules.cpp` is the full reference — mechanics,
costs, tradeoffs, helpers, and how to read the telemetry. Four viruses ship with
different strategies; edit any of them and race them from the SETUP screen.
[`src/games/virus/virus_api.h`](src/games/virus/virus_api.h) is the API it
describes.

## Layout

```
src/
  main.cpp        boot, game registry, main loop
  core/           hardware + shell: display, controls, storage, System, the Game interface
  ui/             menu, splash, settings, scoreboard, shared chrome (widgets)
  net/            the ESP-NOW stack: proto, transport, session/sync, NetGame
  match/          match runners on that stack: multiplayer, single-player vs AI,
                  plus the countdown/result timing all runners share
  games/
    tetris/       sim (rules) split from the Game that renders it
    breakout/     same split: pure fixed-point physics, plus the Game around it
    tron/         the multiplayer vertical slice
    swarm/        co-op wave shooter: same sim/Game split, plus the wave table
    virus/        referee, author API, and the rules you edit
docs/
  architecture.md the living reference for the network stack — keep it current
  plans/          completed design records, not maintained
test/
  shims/          Arduino/FastLED stand-ins for the native build
  stubs.cpp       no-op Display, fake clock and fake pins, shared by every suite
  test_controls/  joystick deadzone, centre calibration, step hysteresis, repeat
  test_tron/      Tron sim, incl. heading input and reversal rejection
  test_virus/     Virus referee tests
  test_tetris/    7-bag fairness, gravity, lock delay, DAS, hard drop, scoring
  test_breakout/  Breakout physics and the velocity paddle, at 8×8 and 16×16
  test_swarm/     every weapon kills every alien, no friendly fire, the wire
```

Adding a game means implementing `Game` (or `NetGame` for a networked one) and
registering it in the `MENU[]` table in [`src/main.cpp`](src/main.cpp).

## Where to read next

- [`docs/architecture.md`](docs/architecture.md) — why ESP-NOW, the four network
  layers, what crosses the wire, and the host-authoritative model.
- [`docs/plans/`](docs/plans/) — the design records behind decisions already
  shipped. Point-in-time; their file paths predate the current layout.
