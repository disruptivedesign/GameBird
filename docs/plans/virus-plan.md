# Virus — Implementation Plan (single-player v1)

Status: **implemented (single-player v1)** — compiles clean for both PlatformIO envs; on-hardware
tuning pending. Targets the current 8×8 panel; multiplayer later.
Shipped tick cadence is `VIRUS_TICK_MS = 180` (~5.5 Hz) rather than the 4 Hz first sketched — a dial
to revisit once it's on the panel.
Design source: the Virus gameplay spec (Level 1 ruleset) plus the review Q&A that trimmed it for a
mechanical-engineer audience.

## Goal

A programmable cellular game where each player writes a **pure `decide()` rule** compiled into
firmware. The referee runs that rule once per owned cell per tick and applies all funded actions
simultaneously. Own the most cells when the match ends. v1 ships **single-player**: author up to
four viruses, pick which of them fight, then watch them play out on the same board.

The teaching constraint is *locality* — a rule sees only its cell and four orthogonal neighbours.

## Decisions

| Topic | Decision |
|---|---|
| Board size | **8×8 now** (64 tiles). Resolution-agnostic (`begin(arenaW,arenaH)`), so the 16×16 next-gen panel works later with only retuning. |
| v1 scope | **Author up to 4 viruses + pick the matchup.** 2–4 selected viruses play an autonomous match. |
| Sim contract | Virus is a **`NetGame`** (deterministic, host-authoritative, tick-based) — same contract as Tron. Readies multiplayer for free. |
| Live input | A "player" is still a compiled `decide()`, not a joystick. **Button A rides along as `World.button`**, though: a live level, the same for every cell of every virus this tick, so a rule CAN react to a human press without breaking purity (it is handed a value, not remembering one). Buttons otherwise = navigate/exit/rematch. |
| Rule shape | `decide()` is a **pure** function of `(Cell, World)`. No `static`/global state (documented rule). |
| `World` fields | `{ phase, button }` — `phase` is a `Phase` enum (Early/Middle/End) blended from elapsed-time% and board-filled% (max of the two, 33/66 cutoffs); `button` is `ButtonState{ held }`. Dropped `tick`/`budget`/`player_count` (a raw counter and two constants) and never had `my_cells` — keeps locality airtight and gives authors deliberate, actionable signals rather than raw board access. |
| Future signaling | Reserve a **dark `note` byte** in `Cell`/`Neighbour`/`Decision` now, unused in v1, so per-cell signaling later is purely additive. |
| Ergonomics | English-reading helpers (`isEmpty()`, `isEnemy()`, `canGrow(d)`) so authors edit intent, not enum ceremony. |
| Opponent bots | The reference bot ships as the **default body of every unedited `decide()`**, so any un-authored virus still plays. |

---

## 1. Architecture

Virus is a `NetGame` (see [net_game.h](../src/net_game.h)) — the same host-authoritative, seeded,
resolution-agnostic contract Tron uses. All gameplay lives in the referee (`hostTick`), so it is
deterministic and reproducible by construction, exactly as the spec demands.

**The one departure from Tron:** input. A Virus player is a compiled function, so the live-input path
(`serializeInput`/`applyInput`/`aiInput`) is unused during play. The referee calls each player's
`decide()` inside `hostTick()`. Because there is no live human control and there is a pre-match
selection step, Virus gets its own thin app rather than reusing the `SinglePlayer` runner:

```
VirusApp (new)                         Virus (new NetGame — the referee)
  SETUP:  pick 2–4 viruses    ───────► begin(8,8, roster, seed, colors)
  each tick (autonomous):
    hostTick()                ───────► snapshot → decide() per cell → fund →
                                        validate → combat → claims → commit
    render(disp)              ───────► owners as colors, strength as brightness
  RESULT: win tally + rematch ───────► reuses scoreboard.drawWinBars()
  after N rounds: SERIES_OVER  ───────► final standings, champion framed; A=new series, B=picker
```

A **series** is `VIRUS_SERIES_ROUNDS` (default 5) matches on the same roster. Matches auto-rematch
until the count is reached, then the app stops on a final standings screen (champion's bar blinking
and framed in its color) instead of looping forever. `A` starts a fresh series, `B` re-picks.

Reused verbatim: the `NetGame` contract, `scoreboard.drawWinBars` ([scoreboard.h](../src/scoreboard.h)),
the countdown/result pattern, `PRESET_COLORS`, and the `MENU[]` registration flow.

## 2. The referee — `hostTick()` (spec §3, six phases)

One authoritative tick reads a **snapshot** taken before phase 1 and applies all writes at commit
(standard CA double-buffer — the determinism invariant):

1. **Decide** — for each player, for each owned cell in row-major scan order, call `decide()`. User
   code cannot mutate anything.
2. **Fund** — per player, sort that player's actions by `priority` desc (ties → scan order), spend
   `budget` down the list, discard the rest. `Idle` costs 0.
3. **Validate** — drop illegal actions against the snapshot; **no budget refund**.
4. **Combat** — apply all `Attack` damage vs snapshot strength (mutual kills possible).
5. **Claims** — resolve `Grow` onto Empty/Neutral; contested tiles annihilate.
6. **Commit** — apply `Fortify`, clamp strength to `[0, STRENGTH_MAX]`, 0-strength → Neutral, write
   the new board, `tick++`.

No floating point, fixed scan order, no unordered iteration — all per spec §11. ≤64 `decide()` calls
per tick is negligible on the ESP32-S3.

## 3. Rule API (`virus_api.h`) — the only header the author sees

```cpp
enum class Occupant : uint8_t { Empty, Neutral, Self, Enemy, Wall, Edge };
enum class Dir      : uint8_t { N, E, S, W };

enum class Strength : uint8_t { None, Weak, Normal, Strong, Strongest }; // ordered; compare directly
struct Neighbour {
    Occupant occupant;
    Strength strength;   // Weak..Strongest when Self/Enemy; None otherwise
    uint8_t  enemy_id;   // 0 unless Enemy
    uint8_t  note;       // RESERVED, always 0 in v1
    bool isEmpty() const; bool isEnemy() const; bool isSelf() const; // + isNeutral/isWall/isEdge
};
struct Cell {
    Strength  strength;
    uint8_t   note;                        // RESERVED, always 0 in v1
    Neighbour n[4];
    const Neighbour& neighbour(Dir d) const;
    bool canGrow(Dir d) const;             // neighbour is Empty or Neutral
    // + countAdjacent(Occupant), findOpen(Dir&), findWeakestEnemy(Dir&)
};
enum class Priority : uint8_t { Low, Medium, High };  // mixed case: Arduino reserves LOW/HIGH
enum class Phase    : uint8_t { Early, Middle, End }; // max(time-elapsed%, board-filled%): 33/66 cutoffs
struct World  { Phase phase; uint16_t energy; bool canAfford(const Action&) const; };
struct Decision { Action action; Priority priority; uint8_t note; };
// Action::cost() -> energy cost (Grow 2, Fortify/Attack 1, Idle 0)

Decision decide(const Cell& me, const World& world);   // the one function an author writes
```

Actions (Level 1): `Idle`(0) · `Grow(d)`(2) · `Fortify`(1) · `Attack(d)`(1) · `Spore(d)`(6). Growth
costs double because area is the score. Reference bot = expand where possible, else fortify. Priority is a
three-level enum (`Low`/`Medium`/`High`) rather than a raw number — enough control for the audience,
and the referee funds `High` first, ties broken by scan order.

**Spore** seeds a new cell **three steps** away in a direction (`VIRUS_SPORE_REACH`), at `Weak`,
leaving the parent untouched.
Only the *landing* tile must be open — whatever sits between is irrelevant, so a spore leaps enemy
cells, your own cells, and (Level 2+) walls. It resolves through the same claim map as `Grow`, so a
spore and a grow racing for one tile annihilate exactly as two grows would.

At **3× the cost of `Grow`** it is never an efficiency play — the same 6 energy buys *three* adjacent
cells by growing. It is priced purely as a way through a blockade:
- **It crosses ground you don't control.** This is the whole point — it's the only way to get past a
  blockade without fighting through it, and the only reason to ever pay 6 for one cell.
- **It's a banked-energy play.** At 6 against 3/tick income it takes two full ticks of savings, which
  pairs with the accumulating pool (cap 15 = two spores with change): a boxed-in virus stops
  spending, banks, then bursts out.
- **It breaks stalemates.** A frozen frontier stays frozen because neither side can gain ground;
  sporing past the seam lands a beachhead behind it. Expect this to reduce how often the windowed
  stall detector has to fire.

Because a cell can't otherwise see three tiles out, `Cell` gains **`far_open[4]`** — purely "is the
landing tile free?", surfaced as `canSpore(d)` / `findSporeTarget(d)`. This is a deliberate, minimal
widening of the sensing window (5 tiles + 4 availability bits): an action you cannot aim is an action
nobody uses. No strength or ownership detail is exposed at range, and nothing is revealed about the
tiles jumped over.

> On the 8×8, reach 3 spans nearly half the board, so it clears any realistic frontier thickness and
> lands well behind a seam. Edge cells simply have fewer legal directions (the landing tile is off
> the board), which `canSpore` reports honestly.

**Energy** is an accumulating, capped pool (`VIRUS_ENERGY_PER_TICK` = 3 per tick, banked up to
`VIRUS_ENERGY_MAX` = 12) — one pool per virus, shared by all its cells. Each tick the referee adds
the income, exposes the pool as `world.energy`, funds the virus's actions High→Low until it runs
out, and banks the remainder. `world.canAfford(action)` lets a rule check the pool (advisory — the
referee still arbitrates by Priority). Accumulation favours the underdog: a large virus spends every
tick and never banks, while a boxed-in one saves up for a burst — an anti-snowball lever, tunable via
the cap.

## 4. Authoring & selection (v1 UX)

- **`virus_rules.cpp`** holds four functions `decideA/B/C/D`, prefilled with four *distinct*
  documented strategies — Balanced (reference), Fortress, Aggressor, Spreader — so an out-of-box
  match is lively and each is a worked example to learn from. An author edits one or more; unedited
  ones still play. This is the *only* file put in front of an author — everything else is locked and
  out of view. Cell helpers (`findOpen`, `findWeakestEnemy`, `countAdjacent`) keep rule code readable.
- **Selection (VirusApp `SETUP`):** four colored letters A/B/C/D; each lit = in, dim = out. Slider
  highlights, A toggles, Start requires ≥2 in. Each virus keeps a fixed preset color across matches.
  *(Assumption to confirm: a virus appears at most once per match — no duplicate-slot matchups yet.)*

## 5. Rendering on 8×8

Owner → player color; strength 1–4 → four brightness steps of that color (`{35,100,180,255}`, spread
wide so all four tiers stay distinguishable at the panel's low global brightness — this is what makes
fortifying and being worn down readable tick over tick); Neutral → dim gray; Empty → off. Reads
cleanly at 2 players; 4 is busy but legible. Uses `Display::setPixel` / `border`
([display.h](../src/display.h)).

### Combat feedback (render-only)

On top of the steady state, cells **pulse** on the tick something happened to them: took damage and
survived → red; died → white, fading into the scorched gray. Both fade out over `VIRUS_FX_MS` (60 ms,
a third of a tick) so they read as a pulse rather than a strobe — `render()` runs every main-loop
pass (~5 ms) against a 180 ms tick, so there are ~36 frames to fade across.

The referee records this in a per-cell `_fx` byte during `commitTick()` (where damage is already
resolved), plus a `_tickWallMs` stamp for the fade phase. Both are **render-only**: never read by the
simulation, never serialized, rebuilt every tick — determinism does not depend on them.

Deliberately *not* flashed: attacking and fortifying. On a contested frontier nearly every cell
attacks every tick, so an attacker flash becomes a permanent panel-wide strobe that drowns out
ownership rather than marking an event; fortifying is already visible via the brightness ramp.

> Note: red-on-orange and red-on-magenta differ by only one channel dropping out, so the damage pulse
> may read as a shade shift for those two players. If it doesn't land on hardware, the fallback is to
> blend toward black instead of red — a dropout reads as a hit against any owner color.

### Spawns (seed-varied)

Each player owns a board quadrant (order spreads them out — 2 players land diagonally) and drops its
seed cell at a **seed-chosen position inside its quadrant**, inset one tile from the edge. Fixed
corners would replay a bit-identical match every time, because the rules are position-blind and the
board is symmetric — so rematches and best-of-N would be meaningless. Randomising within the quadrant
(via a local xorshift32 seeded from the match seed) varies every match while staying fully
reproducible from `(seed, roster)`. `esp_random()` supplies a fresh seed per match.

### Author feedback (Serial telemetry)

Guarded by `VIRUS_TELEMETRY` (default on), the referee prints one line per tick over USB
(115200) — each virus's letter, cell count, total strength, and funded/wanted action counts
(`act=1/4` = wanted 4 moves, budget covered 1). This is the iterate-on-`decide()` feedback loop;
set `-DVIRUS_TELEMETRY=0` to silence.

## 6. Tunables — starting guesses for 64 tiles (tune by watching)

| Constant | v1 start | Note |
|---|---:|---|
| `BOARD` | 8×8 | from `MATRIX_W/H` |
| `ENERGY_PER_TICK` | 3 | energy each virus gains per tick; sets how long the map stays open |
| `ENERGY_MAX` | 12 | cap on banked energy (~4 ticks); how big a saved-up burst can get |
| `MATCH_TICKS` | 160 | ~40 s at 4 Hz; sets war length |
| `TICK_HZ` | 4 | via `match_config.h` cadence |
| `START_STRENGTH` | 1 | diagonal starts → no early threat, 1 is cleaner |
| `STRENGTH_MAX` | 4 | |
| `COST_GROW / FORTIFY / ATTACK` | 2 / 1 / 1 | area is the scoring resource |
| `DEATH_LEAVES_NEUTRAL` | 1 | attacker must `Grow` into a kill separately |

These are derived, not playtested — expect to retune on-device, and confirm with an off-device
tournament runner before trusting balance (deferred; see §8).

### Stalemate detection

The match also ends early once **no player's cell count has changed by more than `VIRUS_STALL_EPS`
(1) over the last `VIRUS_STALL_WINDOW` (24) ticks**. Each tick the referee compares the current
per-player counts against their values one full window ago (a small ring buffer) and ends the match
if the max change is within EPS.

Comparing *net* counts over a window — rather than the owner map tick-to-tick — is deliberate: a
frozen frontier and a *flickering* one both need to end, but they look different tick-to-tick. A
flickering seam (cells killed at strength 1 leave Neutral ground that gets reclaimed next tick, over
and over) changes the owner map every tick, so a tick-to-tick check never fires; but it nets zero
territory change, so the windowed count check catches it. The metric is amplitude-independent —
however many cells churn, if no ground is *net* changing hands the game is decided. The per-tick
`netD` is shown in the Serial telemetry so the threshold is easy to tune.

## 7. File-by-file

| File | Change |
|---|---|
| `src/virus_api.h` | **new** — Occupant/Dir/Neighbour/Cell/World/Decision/Action + helper predicates |
| `src/virus_rules.cpp` | **new** — the author-editable file: `decideA/B/C/D`, default = commented reference bot |
| `src/virus.{h,cpp}` | **new** — the `NetGame`: board + snapshot, 6-phase referee, render, roster→rule table, scoring, menu icon |
| `src/virus_app.{h,cpp}` | **new** — SETUP (virus toggle) → COUNTDOWN → PLAYING (autonomous) → RESULT (tally, rematch); B exits |
| `src/main.cpp` | construct `Virus` + `VirusApp`; add to `MENU[]` |
| `src/scoreboard.{h,cpp}` | reuse `drawWinBars` (no change) |

No wire-protocol change (single-player is offline). Existing games untouched.

## 8. Build order

1. `virus_api.h` + `virus_rules.cpp` scaffold (four reference bots) — compiles, no sim.
2. `virus.{h,cpp}`: board state + static render — see a fixed board on hardware.
3. Referee `hostTick()` (six phases) + `isOver`/scoring.
4. `VirusApp` SETUP screen (virus toggle selection) + colors/roster wiring.
5. `VirusApp` autonomous match + countdown/result/tally; register in `MENU`.
6. Tune §6 by watching on the 8×8.

## 9. Later (planned, not v1)

- **Levels / terrain:** a new `Occupant` value + one referee rule; author `decide()` code keeps
  compiling. Walls (impassable chokepoints), Nutrients (+budget to holder — the snowball lever, tune
  carefully), Vaccine (hazard). Layouts are the only match-to-match variation.
- **Tier-2 actions:** `Move`, `Donate`, `Split`, `Apoptosis`, `Infect`.
- **Per-cell signaling:** light up the reserved `note` byte for gradients/waves.
- **Multiplayer:** Virus is already a `NetGame` → slots into the `Multiplayer` controller; new work
  is selection/lobby UI, not sim.
- **Off-device tournament runner:** deterministic ladder for balance tuning (the on-device watch mode
  is for players, not sweeps).
- **Duplicate-slot matchups** (same virus in multiple corners), if wanted.
