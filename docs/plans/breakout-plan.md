# Breakout

A single-player Breakout for the LED panel: brick rows at the top, a
slider-driven paddle at the bottom, one ball, three lives.

The interesting problem is not Breakout. It is Breakout **on 64 pixels**, on a
panel that is about to become 256.

## What the arcade game actually is

Atari's 1976 original, reduced to the parts that matter:

- A ball bounces in a walled box. Bricks fill the top, the paddle guards the
  bottom. Hit a brick, it dies and scores. Miss the ball, lose one of three.
- **Paddle English is the game.** The outgoing angle depends on *where on the
  paddle the ball lands*, not on the incoming angle. Remove it and Breakout
  stops being a game — you just park the paddle under the ball and wait.
- Difficulty is emergent, not levelled. Four escalations do all the work:
  the ball speeds up after the 4th hit and again after the 12th; it speeds up
  again on reaching the top rows; and the paddle **halves in width** the first
  time the ball touches the top wall.
- Deeper rows score more, so the cheese strategy — tunnel up a side, trap the
  ball above the bricks — *is* the intended high-scoring strategy. That is the
  design lesson worth copying.

Two rules exist purely to prevent degenerate states, and every implementation
rediscovers them the hard way:

1. The angle is never allowed to go flat. A near-horizontal ball ping-ponging
   between the side walls is unrecoverable and unwatchable.
2. Deflection changes direction only, never speed. Constant magnitude.

## The 8x8 problem

64 pixels is a brutal budget. The layout:

```
row 0-1   bricks     16 bricks, 2 rows
row 2-6   air        5 rows of travel
row 7     paddle     3 cells wide, 6 positions
```

**The ball cannot live on the integer grid.** A ball moving +/-1 cell per step
only ever travels at 45 degrees, only visits one parity of cells, and has
exactly two possible directions — so paddle English is not expressible and
Breakout dies. This is the decision the whole design hangs off.

So the ball is a **sub-pixel object rendered to the nearest cell**: position
and velocity in Q8 fixed point (256 = one cell), integer math throughout, no
floats and no trig. That single choice buys real angles, English, and a speed
ramp for nothing.

On top of it, velocity is not a free vector. It is drawn from a table of **six
unit directions** — roughly +/-20, +/-45, +/-65 degrees off vertical — scaled
by a speed scalar:

```
_dir   0..5, the horizontal fan (left-steep to right-steep)
_up    travelling up or down
_speed magnitude, Q8 cells per tick
```

Every reflection is therefore a table operation, not arithmetic:

| Event | Effect |
|---|---|
| Side wall, or a brick's vertical face | `_dir = 5 - _dir` |
| Top wall, or a brick's horizontal face | `_up = !_up` |
| Paddle | `_dir = zone(ball_x - paddle_x)`, `_up = true` |

This is worth more than tidiness. There is deliberately no 0-degree entry and
nothing flatter than 65, so both anti-degenerate rules are **structural** — the
ball cannot represent a stuck angle — rather than a clamp bolted on afterwards.
Reflections are exact: no rounding drift, no energy loss, and the physics is
exactly reproducible in a test.

### What was cut

- **Two screens then game over** -> endless levels. 16 bricks clear far too
  fast to end a game after two screens.
- **Power-ups** -> those are Arkanoid, not Breakout.
- **A lives HUD** -> no room. Lives flash as N pixels before each serve.

Paddle shrink and the speed schedule both survive intact, are nearly free, and
are most of the difficulty curve.

## Collision

Speed is invariably below one cell per tick, so collision is a two-pass
axis-separated step with no sweep and no tunnelling:

1. Move X. Reflect off a side wall if the new position is out of bounds.
   If the new cell holds a brick: destroy it, `_dir = 5 - _dir`, and *do not*
   commit the X move.
2. Move Y. Reflect off the top wall. Check the paddle plane. If the new cell
   holds a brick: destroy it, flip `_up`, and do not commit the Y move.

The safety of the "reflect off the wall, then test the cell we landed in" order
rests on one invariant: **the ball is never inside a brick cell**, because
bricks are destroyed the instant the ball enters them. A wall reflection puts
the ball back in the cell it already occupied, which is by definition
brick-free — so the immediately following brick test cannot false-positive.
Tunnelling along the top wall, the whole point of the cheese strategy, falls
out of this for free.

The paddle catches by **plane crossing**, not by cell overlap: when the ball
crosses `y = (h-1) * 256` moving downward, it is caught if the ball's X lies
within the paddle's span. A miss lets the ball travel visibly into the paddle
row and out of the bottom, which reads correctly as "it got past you".

The invariant the collision model depends on is `speed < 256` (one cell per
tick). Max component is `speed * 241 / 256`, so speed is clamped to 224 and
[`test_breakout`](../../test/test_breakout/test_breakout.cpp) pins it.

## Scaling to 16x16

The sim takes `(w, h)` in `newGame()` and **never references `MATRIX_W` /
`MATRIX_H`**, the same discipline `Tron` uses. Everything derives:

| | 8x8 | 16x16 |
|---|---|---|
| Brick rows `max(2, h/4)` | 2 | 4 |
| Paddle `max(2, w*3/8)` | 3 | 6 |
| Bricks | 16 | 64 |
| Point tiers | 1, 3 | 1, 3, 5, 7 |
| Ball speed `base * h/8` | 1x | 2x |

That last row is the one that is easy to miss. Holding speed constant in
cells-per-tick makes the 16x16 board feel *half as fast*, because the ball has
twice as far to travel. Scaling with height keeps the timing identical, so the
game feels the same on both panels and only looks more detailed on the larger
one. At 16x16 the point tiers land exactly on Atari's 1/3/5/7.

Bricks are `uint16_t rows[8]`, one bit per column — already 16-wide, no
reallocation.

The real work in the 16x16 upgrade is not here. It is `defines.h` (256 LEDs,
the `DEAD_INDEX` bypass, RGBW buffer sizing), the power budget, and
`Display::drawBitmap`, whose `const uint8_t[8][8]` signature and the `Icon`
struct hard-code an 8x8 menu icon. Breakout will follow whatever those become.

## Structure

```
src/games/breakout/
  breakout_sim.h/.cpp   pure rules + fixed-point physics. No Arduino, no
                        Display, no audio -- parameterised by (w, h) and
                        compiled by the native test env.
  breakout.h/.cpp       the Game: menu, serve/play/lost/clear/over states,
                        slider and buttons, rendering, audio, high score.
  bricks.h              icon, palette and row colours (data only, as pieces.h
                        is to Tetris).
test/test_breakout/     the sim's tests, run at BOTH 8x8 and 16x16.
```

The split is the point. Breakout's physics is the first thing on this device
with behaviour that is genuinely hard to eyeball — angle clamping, tunnelling,
the no-false-positive invariant above — and it is the thing the panel upgrade
is most likely to silently break. Running the identical suite at 8x8 and 16x16
is what makes the upgrade a configuration change instead of a bring-up.

The sim never plays a sound or draws a pixel. `step()` returns a bitmask of
what happened (`BRK_EV_BRICK`, `BRK_EV_PADDLE`, `BRK_EV_LOST`, ...) and the app
layer maps those to the `sfx.h` catalog. That is what keeps the sim linkable in
the native env.

## Serve

The ball parks on the paddle and rides it; A launches. The launch angle comes
from the paddle's position across the board, mirrored — far left serves steeply
right, centre serves near-vertical — so the serve always opens into free space
and is already a decision. It also gives the player a beat to recover after
losing a ball, and keeps A as the action button it is in Tetris.

## Feel constants

Tuned on 8x8; all of them live at the top of `breakout_sim.cpp`.

| | |
|---|---|
| Tick | 16 ms (~62 Hz), fixed-step accumulator |
| Base speed | 30 Q8/tick at h=8 (~7.3 cells/s) |
| Speed steps | x1.00, x1.25, x1.50, x1.75 |
| Speed-ups at | 4 hits, 12 hits, first top-row brick (cap 3) |
| Paddle shrink | first top-wall touch, `w -> max(2, w/2)` |
| Lives | 3 |
| Level | +10% base speed per level, capped |

## Deliberately not done

- **No brick health or multi-hit bricks.** Two visible rows on 8x8 means a hit
  must read as a kill; anything else is invisible.
- **No ball trail.** Tried on paper; on a 5-row playfield it reads as a second
  ball.
- **No two-player alternating.** The device has one slider.
