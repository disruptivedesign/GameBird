# Virus v2 — Move, and per-cell energy

Status: **implemented** — native suite green (101 cases, 15 of them Virus), both PlatformIO envs
build clean, tunables swept off-device against the four starters; on-panel confirmation pending.
Targets the shipped 16×16 rev 2 board (a 16×14 Virus arena, 224 tiles).
Source: playtest feedback from two authors writing viruses against v1, plus the decisions taken in
response (§0). Supersedes parts of [virus-plan.md](virus-plan.md) — specifically the `World` shape,
the energy model, and the funding walk.

## 0. The feedback, and what we decided

Two notes came back from authors using [`virus_rules.cpp`](../../src/games/virus/virus_rules.cpp):

> I'd add a move action, if only because it makes for fun displays as your virus moves around
> (conway's game of life kinda thing)

> Having energy being global makes it somewhat hard to reason about what my cell is going to do
> immediately. The `decide` function is called on my Cell but I have to make a decision globally for
> my "team" without knowing where I am in the iteration. Usually in these games, I've seen energy as
> per-cell. Either way could be fine — but in its current state I think I'd want the `decide` function
> to have a bit more info about where I am in the iteration (or maybe have the decide happen over my
> whole slice of cells)

The second note is a fair hit on a decision that was made deliberately. v1 gave every cell the whole
team purse (`world.energy`) and no idea of its position in the walk, then made the walk **stop** at
the first unaffordable action — so one badly-guarded request starved every cell behind it, and an
author had to guess their own cell count to know whether a request was safe. `canAfford()` was
advisory and had to be documented as such. That is the thing that is hard to reason about.

| Topic | Decision |
|---|---|
| Energy | **Per-cell allowance.** Each cell earns and banks its own share of a board-fixed income and spends only from its own purse. The team pool and `Priority` are retired. |
| Funding | Falls out of the above: no walk, no ordering, no starvation. `canAfford()` becomes an exact guarantee. |
| Move | `Action::move(d)`, **cost 1, vacated tile returns to Empty.** Carries the cell's strength *and* its banked energy — it is the same cell, relocated. |
| Stalemate | **Movement counts as progress.** The detector gains a centroid-drift term so a kinetic match plays to the clock. |

Considered and rejected: adding `my_cells`/`cell_index` to `World` while keeping the shared pool
(cheaper, but leaves the trap and the advisory `canAfford` in place); true per-cell income
(conventional, but income would scale with size and snowball, which v1's board-fixed income exists to
prevent); a slice-level `decide()` over all your cells at once (most powerful, but abandons the
locality constraint the game is built on).

---

## 1. Energy: per-cell, accumulating, board-fixed in total

### The problem with a flat division

Income on 224 tiles is `224 * 3 / 64 = 10` per tick. Handing each cell `pool / my_cells` gives a
40-cell virus `10 / 40 = 0`. Every cell gets nothing and the match stops. So the share has to
**accumulate**: a cell earns a fractional trickle every tick and banks it until it can afford
something.

### The model

Per player, income is unchanged and still derived from the **board**, not the virus's size
(`incomeFor()`), so a big virus does not out-earn a small one. Each tick that income is split evenly
across the cells the virus owns *at snapshot* and credited to each cell's own bank, in units of
1/256 energy to avoid floating point:

```
share_q8 = (income * 256) / my_cells        // floor; deterministic
cell.bank = min(cell.bank + share_q8, VIRUS_CELL_BANK * 256)
```

`me.energy` exposes `bank >> 8` — whole energy the cell can spend right now. A cell spends from its
own bank and from nothing else, so:

- **`me.canAfford(a)` is now exact.** If it is true, the action *will* be funded. No other cell can
  take the energy first. This is the whole point of the change.
- **There is no funding walk.** No sort, no priority, no stopping, no starvation. `decide()` runs
  once per cell in scan order and each result is funded or not on the spot, independently.
- **Scan order stops mattering** for anything except which of two of your own cells wins a contested
  claim.

| Constant | v1 | v2 | Why |
|---|---:|---:|---|
| `VIRUS_ENERGY_BASE` | 3 | **4** | measured; see below |
| `VIRUS_ENERGY_BANK` (team, ticks of income) | 5 | *removed* | no team pool |
| `VIRUS_CELL_BANK` (per cell, whole energy) | — | **8** | one Spore (6) plus a Grow (2) |

### The cost of losing Priority, and what it actually cost

In v1, Priority let a virus route **100% of its income to the frontier** — interior cells returned
`Idle` or `Low` and the energy went where it mattered. Per-cell shares pay every cell equally,
including interior cells with nothing to spend on, which fill their bank to `VIRUS_CELL_BANK` and
then waste the rest. The plan called for doubling the income to 6 to compensate.

**Measured, that compensation is not needed.** Sweeping `VIRUS_ENERGY_BASE` from 2 to 6 against the
four starters on 16×14 (six seeds each, off-device) puts the board at ~90% occupancy at *every* value
in the range, and every match ends on the stalemate detector between t≈65 and t≈160 of a 240-tick
clock — the same window v1 ended in. Income turns out to buy **pace**, not reach: 2 takes about a
third longer to arrive at the same board as 6. Settled on **4**. The constant is now `#ifndef`-guarded
so it can be swept with `-DVIRUS_ENERGY_BASE=n` rather than edited.

### Does a per-cell economy flatten strategy?

Worth checking, because equalising the money across cells could plausibly equalise the outcome
regardless of what a rule says. It does not: a deliberately bad rule (fortify in place, never expand)
is reduced to **1 cell** by the three starters in both v1 and v2, and with it removed from
contention the remaining three finish 55–92 apart. The tight four-way scores in the v2 probe (37–70)
are the four *starters* being closely matched, not the economy erasing the difference — which is a
better balanced default roster than v1's, where D won four of six seeds.

Two side effects worth knowing:

- **Spore changes character.** In v1 any cell could spend the team's banked 15. Now a *specific* cell
  must save 6 out of its own trickle: fast for a small virus (a 3-cell virus banks ~1/tick per cell,
  so ~6 ticks), slow for a large one (~0.3/tick per cell at 60 cells, so ~20 ticks). That is a strong
  anti-snowball, underdog-favouring effect and probably good — but if Spore becomes unreachable for
  mid-size viruses, drop its cost before touching income.
- **A one-cell virus wastes income.** The seed cell caps at 8 and discards the rest until it has
  siblings to share with. This slows the opening, which reads fine on the panel.

### API shape

`energy` moves from `World` onto `Cell`, which shrinks `World` back to the single field
[virus-plan.md](virus-plan.md) originally argued for — locality is airtight again, and each cell now
sees only its own tile, its neighbours, and its own purse.

```cpp
struct Cell {
    Strength  strength;
    uint16_t  energy;          // NEW: this cell's own bank, whole units
    uint8_t   note;
    Neighbour n[4];
    bool      far_open[4];
    bool canAfford(const Action& a) const { return energy >= a.cost(); }   // exact
    // ... existing helpers unchanged
};

struct World { Phase phase; };                       // energy gone

struct Decision {                                    // priority gone
    Action  action;
    uint8_t note;
    Decision(Action a) : action(a), note(0) {}       // so a rule can `return Action::grow(d);`
};

enum class Priority { ... };                         // DELETED
```

Making `Decision` constructible from an `Action` means a rule body reads `return Action::grow(d);`
rather than the brace-plus-priority ceremony, which is a real readability win for the audience.

---

## 2. Move

```cpp
Action::move(d)   // cost 1: relocate this cell one step, carrying strength and banked energy
```

### Rules

- Legal when the neighbour in `d` is **Empty or Neutral at snapshot** — the same test as `Grow`, so
  `canGrow(d)` answers "can I move there?" too.
- Resolves through the **same claim map** as Grow and Spore. A Move contesting a Grow annihilates
  exactly as two Grows do.
- **Atomic.** If the claim is lost (contested, or another of your own cells claimed the tile first),
  the mover *stays put* and has paid for nothing. A contested move must never delete a cell.
- **Death wins.** If the mover takes lethal damage the same tick, the move is void and the tile stays
  open. Attackers hit where it was.
- The destination receives the mover's post-combat strength and its **banked energy**; the origin
  returns to **Empty** (nothing died there, so no scorching) with a zeroed bank.

### The two things authors must be told

1. **Chain moves are impossible.** The destination must be open *at snapshot*, so a cell cannot move
   into a tile a teammate is vacating this tick, and two cells cannot swap. This is the
   double-buffering invariant, not a bug — but conga lines do not work and the tour has to say so.
2. **Moving is not growing.** Area is the score; a move nets zero area. It repositions.

### Referee changes

`commitTick()` currently resolves combat and claims in one pass over the board. Move needs "did the
source survive?" answered before the destination is written, so it becomes three passes over disjoint
tile sets:

- **A — fates.** For each tile owned at snapshot: `died[c] = (_damage[c] >= _sStr[c])`, and
  `newStr[c] = clamp(_sStr[c] - _damage[c] + _fort[c])`.
- **B — claims** (tiles that were Empty/Neutral). Single uncontested claimant takes it. If the claim
  came from a Move (`_claimSrc[c]` set), and the source died in pass A, the tile stays open and
  nothing is vacated; otherwise the tile takes `newStr[src]` and `bank[src]`, and `vacated[src] = 1`.
  Grow/Spore claims land at strength 1 with a zero bank, as now.
- **C — owned tiles.** Died → Neutral + `FX_DIED`; vacated → Empty; else keep owner at `newStr` with
  `FX_DAMAGED` if it took a hit.

B must run before C because B reads `bank[src]`/`died[src]` for tiles C overwrites.

New scratch: `_claimSrc[VIRUS_MAX_CELLS]` (uint16, 0xFFFF = none), `_died[]`, `_newStr[]`,
`_vacated[]`. `_claimSrc` is written **only** on the `_claimBy` 0→claimant transition, so when two of
your own cells move into one tile exactly one of them vacates. Deleted: `PendingAction`, `_pa[]`, and
the insertion sort.

`_cellBank[VIRUS_MAX_CELLS]` (uint16, Q8) is new host-only sim state — zeroed in `begin()`, on death,
on vacate, and for every newly grown or spored cell. It is **not serialized**: clients render the
board and never simulate, so the wire format is untouched and
`test_16x16_snapshot_fits_one_frame` still holds at 197 bytes.

### Feedback surfaces

- `TickEvents` gains `moved`; `VirusVoice` gains `onMove`. Slot it into `playTickVoices()` between
  `damaged` and `attacked` — a move is a smaller event than taking a hit, bigger than a grow.
- Render: no change needed. A cell that moves just appears in its new tile next tick, which is the
  effect the feedback asked for.

---

## 3. Stalemate: movement as progress

`evaluateEnd()` ends the match when no player's **net cell count** has moved beyond `_stallEps` over
`VIRUS_STALL_WINDOW` (24) ticks. Moving cells do not change any count, so a purely kinetic match —
exactly the display Move exists to produce — would be called stalled after 24 ticks.

Fix: track a **centroid** alongside the count. Per player per tick, ring-buffer `cells`, `sumX` and
`sumY` (max 224 × 15 = 3360, fits `uint16_t`). A match is stalled only if, for every player, *all
three* are flat against their value one full window ago:

```
|cells_now - cells_then|            <= _stallEps
|meanQ4(sumX,cells) - meanQ4_then|  <  VIRUS_STALL_DRIFT_Q4     // 16 == one tile
|meanQ4(sumY,cells) - meanQ4_then|  <  VIRUS_STALL_DRIFT_Q4
```

`meanQ4 = sum * 16 / cells` keeps sub-tile drift visible where an integer mean would truncate it away.
Chosen over "count the moves" because a virus oscillating two cells forever would keep resetting a
move counter, but its centroid stays flat — correctly reading as nothing happening — while a drifting
or reshaping swarm moves its centroid and keeps playing. Guard `cells == 0`. Extra cost: two
`uint16_t[24][4]` rings, 384 bytes. `drift=` joins the telemetry line so the threshold is tunable by
watching.

---

## 4. The four starters

All four need rewriting for the new `Decision` shape regardless. Keeping the strategies distinct, and
giving Move a showcase:

| | Was | Now |
|---|---|---|
| **A** Blue | balanced: grow → attack → fortify | unchanged in spirit; now with exact `canAfford` |
| **B** Orange | fortress: thicken to Strong, then expand | unchanged |
| **C** Cyan | aggressor: attack on sight | unchanged; the `canAfford` guard is now a guarantee |
| **D** Purple | spreader: grow, spore when boxed in | **nomad**: expands into open ground, but the moment an enemy turns up next door it **walks away** rather than trade blows; spores out of a corner once banked. Gets an `onMoved` voice. |

D is the visible demonstration of Move. Fleeing contact was chosen over "walk toward open ground when
hemmed in" because a hemmed-in cell has no open neighbour to walk into — Move and Grow take exactly
the same target test, so a boxed-in cell cannot move either. Retreat is where Move has a real edge
over Grow: for the same 1 energy an Attack costs, it keeps the cell, its strength and its savings,
and it puts motion right at the seam where the eye already is.

## 5. Docs and tests

| File | Change |
|---|---|
| `src/games/virus/virus_api.h` | `Move` + cost; `Cell::energy`/`canAfford`; `World` → `{ phase }`; `Decision(Action)`; delete `Priority` |
| `src/games/virus/virus.{h,cpp}` | per-cell banks, single-pass decide+fund, `Move` in `applyIfLegal`, three-pass `commitTick`, centroid stall term, telemetry |
| `src/games/virus/virus_voice.h` | `onMove` |
| `src/games/virus/virus_rules.cpp` | full tour rewrite (energy, funding, Move, the two Move gotchas) + four starters + D's voice |
| `src/games/virus/virus_app.cpp` | `moved` in `playTickVoices()` |
| `test/test_virus/test_virus.cpp` | see below |
| `README.md` (~line 115) | the action list, and "plus a priority" |
| `docs/plans/README.md` | index row for this record |

Tests **deleted** (they pin behaviour that no longer exists):
`test_unaffordable_action_starves_the_rest`, `test_high_priority_outranks_scan_order`.

Tests **added**:

- a cell spends only its own bank; a sibling's wealth cannot fund it
- the share accumulates: a cell with a fractional trickle acts every Nth tick, not never
- `canAfford` is exact — true implies the action lands
- Move carries strength *and* bank, and vacates to Empty
- a contested Move leaves the mover in place (the cell-deletion bug this guards against)
- a mover killed on its move tick does not land, and the tile stays open
- no chain move: a cell cannot enter a tile a teammate vacates the same tick
- two of your own cells moving into one tile — exactly one lands, one stays
- a moving-but-not-growing board is *not* called stalled, and *is* once the motion stops

Not covered: the oscillating pair the centroid term is meant to still catch. A rule is pure and
position-blind, so it cannot alternate east/west from one tick to the next — there is no way to
express an oscillator in the rule API. The frozen half of the same test covers the arithmetic.

`test_spore_banks_energy_then_leaps_a_blockade` needs its tick counts recomputed against the per-cell
trickle. `test_16x16_snapshot_fits_one_frame` and `test_state_round_trips_every_cell_value` are
untouched — the wire format does not change.

## 6. Build order

1. `virus_api.h` reshape + the four starters, no referee change yet → compiles, does not run.
2. Per-cell banks and the single-pass decide/fund; delete the walk, the sort and `Priority`. Existing
   tests (minus the two deleted) pass.
3. `Move` — `applyIfLegal` case, `_claimSrc`, three-pass `commitTick`, new tests.
4. Centroid stall term + telemetry.
5. `onMove` voice + `playTickVoices()`.
6. Docs: tour, README, this record's status.
7. Sweep `VIRUS_ENERGY_BASE` off-device (done — see §1) and confirm it on the panel.

## 7. Deliberately not in this change

- Tier-2 actions beyond Move (`Donate`, `Split`, `Apoptosis`, `Infect`).
- Terrain/walls, and the reserved `note` byte — both still dark.
- Any move that is not one orthogonal step (no diagonals, no multi-step dash).
- Serializing per-cell energy. Clients render; they do not simulate.
