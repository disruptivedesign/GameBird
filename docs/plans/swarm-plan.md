# SWARM

A co-operative wave shooter for up to four GAMEBIRDs. Aliens descend, the
squad shoots back, and every player carries a **different weapon** — so what
kills one wave cleanly bounces off the next, and the squad has to cover for
each other.

The interesting problem is not Space Invaders. It is co-op on a **shared 16×16
view** where every player, every alien and every bullet is a single pixel, and
the only vocabulary available for "these two things are different" is colour,
brightness, and the *shape a weapon paints on the grid*.

## Why this game, on this hardware

Everything networked in this tree is PvP: Tron is last-alive, Virus is
territory. There is no mode where the units are on the same side. That gap is
the reason to build this one, and two consequences of the hardware fall in its
favour:

- **The shared view stops being a compromise.** [architecture.md §1](../architecture.md)
  notes the panel is too small for a split screen, so every unit renders an
  identical copy of one world. A PvP game tolerates that. A co-op game *wants*
  it — four people watching the same alien wall converge is the entire appeal.
- **Nobody sits out.** Tron's first casualty watches three other people play.
  Here a death costs the team a shared life and you are back in two seconds.

The rejected alternative was multiplayer tanks. On this panel a tank is one
pixel with a heading that dies on contact, which is Tron with a projectile
bolted on, and the one thing that makes tanks interesting — turret facing
independent of hull facing — is not expressible in a single lit pixel.

## Decisions locked in

| Question | Decision |
|---|---|
| Players | **Up to 4** (the runner supports 5; Swarm caps itself at 4) |
| Weapons | **5 to choose from**, player picks, **duplicates allowed** |
| Lives | **Shared pool** — one team meter, respawn on death |
| Friendly fire | **None.** Ships pass through each other and through friendly shots |
| Run structure | **Fixed run: 6 waves + a boss**, ~4 minutes |
| Series | `seriesRounds() = 0` — the run *is* the match; rematch until somebody backs out |
| Protocol changes | **None.** The weapon rides in the game's own opaque blobs |
| Sim split | Yes — `swarm_sim` pure logic, native-tested, like Tetris and Breakout |

---

## 1. The screen

`arenaH()` returns `MATRIX_H - 1`, following Virus's precedent of a game
telling the runner it wants less than the whole panel ([virus.h:52](../../src/games/virus/virus.h)).
The last panel row is HUD and the runner never hands it to the sim.

```
row  0        alien spawn edge — the swarm enters here
rows 0..10    alien airspace
rows 11..14   player band — 2-axis movement inside these 4 rows
row  15       HUD: shared lives (left) · wave pips (right)
```

**A 4-row band, not a bottom line.** Four ships on one 16-wide row is a
scrum, and with pass-through enabled it reads as a single flickering blob. A
64-cell band spreads the squad out, makes dodging a diver a real manoeuvre,
and gives the Shield role somewhere meaningful to stand.

### Reading the screen

Hue alone will not carry this: five player colours already exist in
[palette.h](../../src/core/palette.h) and two of them (orange, yellow) are the
obvious alien colours. **Brightness is the primary channel:**

| | Rendering |
|---|---|
| Player ships | Full-brightness player colour |
| Player bullets | Player colour, **~60%** — you can always find your own shots |
| Aliens | Fixed hues (deep red, white, violet, amber), **~45%** |
| Blasts / beams | Full white-hot for 2 ticks, then gone |

So everything bright is yours or the squad's, and everything dim is trying to
kill you. That rule survives being glanced at from across a desk, which a hue
rule does not.

**Power.** Worst case is ~52 lit pixels (28 aliens, 4 ships, 20 bullets),
mostly single-channel and mostly dimmed. The laser's full-column flash is 15
pixels in one channel; the bomb's blast is 9. All far inside the budget the
README warns about — a full-screen white fill is the thing to avoid, and
nothing here approaches it.

---

## 2. The five weapons

The differentiation has to be **shape**, not statistics. At 256 pixels nobody
can feel "15% more damage", but a full-column beam versus a 3×3 blast versus a
three-way fan is legible instantly.

| Weapon | A does | Paints | Good against |
|---|---|---|---|
| **Blaster** | fires, ~4/s | 1px bolt, 2 cells/tick | everything, adequately |
| **Laser** | hold ~1 s to charge, release | **the whole column**, pierces, dmg 3 | vertical stacks, high Shooters |
| **Bomb** | fires, ~1.5/s | slow lob, detonates on contact → **3×3**, dmg 2 | clusters, Armour |

The Bomb detonates on the **near face** of whatever it touches, so its nine
cells are centred there: one shot into the underside of a block takes that row
and the row above it, six kills for one trigger pull. Worth stating because the
obvious reading — that it clears any 3×3 you aim it at — is wrong, and the
difference is what stops it being strictly better than the Blaster.
| **Spread** | fires, ~2/s | **3-bolt fan** (↖↑↗), 6-row range | Divers, anything close |
| **Shield** | hold, then release | **3-wide barrier** one row ahead while held; on release, a bouncing **energy orb** | protecting the squad, then clearing it |

**The Shield is the co-op role and needs care.** A pure-support weapon with no
damage output is miserable to hold, so the barrier deals contact damage:
anything that flies into it dies against it. It absorbs alien fire and blocks
divers *for everyone standing behind it* — the only thing in the game whose
value depends on where your teammates are.

> **Revised after the first build.** Holding still raises the barrier, but
> *releasing* now throws it: an energy orb that bounces off all four walls and
> off whatever it kills, living for twice as long as you held. **Only** a
> release throws it — the wind-up outlives the stamina, so once the meter is dry
> the barrier drops and you keep charging with no cover. The last second of
> orb is bought with protection you no longer have, and `SWM_ORB_MAX_HOLD` is
> where the buying stops. It ricochets on
> the vertical only — brick-breaker convention — so it weaves up and down
> through a formation while it crosses, rather than reversing back down its own
> path.
>
> This makes the Shield the only weapon that both defends and clears, and no
> second cost was added for it. The economy already is the cost: every other
> weapon fires continuously, while the Shield spends its stamina and then has
> **no barrier and no attack at all** for the four seconds it takes to refill.
> The orb is what that dead window buys. `SWM_ORB_LIFE_MUL` is the one knob if
> that balance reads wrong on hardware.

**Duplicates are allowed, deliberately.** Silently rewriting a player's
explicit pick — which is what the colour system's next-free-preset dedup would
do — is worse than a sub-optimal loadout. The consequence is a design
constraint: **no alien may be immune to any weapon.** Armour resists bolts and
folds to blasts; it does not ignore bolts. Four players who all pick Bomb get
a hard run, not an unwinnable one.

### Where you pick it

The Swarm submenu, mirroring [TronApp](../../src/games/tron/tron_app.cpp), but
using the second stick axis that submenu leaves idle:

```
stick X   →  1P / MP
stick Y   →  cycle weapon
A         →  enter
B         →  back to the main menu
```

The choice persists via `Storage` under namespace `"swarm"`, key `"wpn"`, so
it is a device preference you set once — exactly how `colorIndex` behaves.

**Switching mid-run:** B during play cycles your weapon, and the host adopts
the change **only at a wave break**. B is genuinely free here —
`tryEndSeriesEarly()` returns false the moment `seriesRounds()` is 0
([multiplayer.cpp:116](../../src/match/multiplayer.cpp:116)), so the press
falls straight through to `LocalInput.b`. This costs nothing to build (the
weapon is already in the input blob every tick) and it is what lets a squad
adapt when the wave table turns against them. Gate it on how it feels on
hardware; the wave-break restriction exists so nobody swaps out of a bad
matchup mid-boss.

---

## 3. The swarm

| Alien | HP | Behaviour | Counter |
|---|---|---|---|
| **Grunt** (deep red) | 1 | marches in formation, descends a row at each edge | anything |
| **Armour** (white) | 3 | marches; **bolts do 1, blasts do full** | Bomb, Laser |
| **Diver** (violet) | 1 | breaks formation, accelerates at the nearest ship | Spread, Shield |
| **Shooter** (amber) | 2 | holds the top rows, fires slow bolts downward | Laser's reach |

Formation movement is the classic one — the whole block steps sideways, drops
a row when either edge is touched, and speeds up as its numbers thin. Divers
and Shooters are drawn from the formation, so killing the block early denies
both.

### The run

Six waves, then the boss. Each wave is a row in a table — a composition and a
spawn cadence — so tuning the difficulty curve is editing data, not code.

| Wave | Composition | ~Length |
|---|---|---|
| 1 | Grunts | 20 s |
| 2 | Grunts + first Divers | 25 s |
| 3 | Grunts + Shooters | 25 s |
| 4 | Armour introduced | 30 s |
| 5 | Divers heavy | 25 s |
| 6 | Everything | 35 s |
| **Boss** | see below | ~60 s |

**Boss.** A 4×3 body, ~40 HP, tracking side to side across the top, firing
patterned volleys and periodically spawning Grunts. One pixel of the body is a
**weak point** taking double damage, and it moves between phases — which gives
the squad something to call out to each other, and gives the Laser and Bomb
distinct jobs (reach it versus splash near it).

Killing the boss ends the run in victory. Exhausting the shared lives ends it
as a wipe.

### Shared lives

Three team lives. A ship that takes a hit costs the team one, then respawns at
its spawn column after ~2 s, blinking, briefly invulnerable. At zero the run
ends immediately, wherever it is.

The HUD's left three pixels are the meter, so the cost of a careless dive is
visible to everybody at the moment it is paid.

---

## 4. Fitting the wire

`NET_STATE_MAX` is 240 B and the snapshot goes out every tick. Positions are
Q4.4 fixed-point in one byte per axis — the same reasoning as Breakout's
sub-pixel ball, since a bullet that can only move whole cells per tick has
exactly one speed.

| Element | Count | Bytes each | Total |
|---|---|---|---|
| Header — phase, wave, lives, numPlayers, bossHp, + three entity counts | — | — | 8 |
| Players — x, y, `weapon:3 alive:1 meter:4`, `invuln:7 shield:1` | 4 | 4 | 16 |
| Aliens — x, y, `type:3 hp:3 flags:2` | 28 | 3 | 84 |
| Bullets — x, y, `kind:2 owner:3 dir:3` | 20 | 3 | 60 |
| Effects — blasts and beams, `cell, kind` | 6 | 2 | 12 |
| **Total** | | | **180 B** |

*(As built. The first draft of this table said 173: it forgot that a
variable-length frame has to carry its own entity counts, and it packed each
player into three bytes before the shield state and the respawn blink needed
somewhere to live.)*

Comfortably inside 240, with Virus's 197 B as the standing precedent that this
model works. As Virus does, the sim `static_assert`s the derivation so the
budget cannot drift unnoticed, and a native test asserts the byte count.

**Input blob — 3 bytes:**

```
byte 0   stickX   int8, -100..100
byte 1   stickY   int8, -100..100
byte 2   A:1  B:1  weapon:3  (3 bits spare)
```

**No protocol change, and no `NET_PROTO_VER` bump.** The weapon travels in the
game's own opaque INPUT blob and comes back in the opaque STATE snapshot, which
is exactly the separation [net_game.h](../../src/net/net_game.h) describes —
the backend never parses either. The alternative considered was extending
`JoinPayload` and `StartPayload` the way `colorIndex` is extended; it was
rejected because it puts a shooter-specific concept into the game-agnostic
protocol, costs a version bump, and would push `StartPayload` to 28 of
`NET_CTRL_MAX`'s 32 bytes for no gain.

The host adopts each player's weapon from the first INPUT it receives after
`begin()`, and thereafter only at wave boundaries.

---

## 5. Winning a game that has no winner

This is the one real point of friction with the existing shell, and it is worth
settling before any code is written.

The match runners are built around a single winner: `isOver(uint8_t& winnerId)`,
`recordWin(winner)`, and a result screen that draws a per-player win-bar chart.
Co-op has no winner. Two specific things go wrong if we ignore that:

1. `enterResult()` plays `SFX_WIN` only to the player whose id matches the
   winner and `SFX_LOSE` to everyone else
   ([multiplayer.cpp:102](../../src/match/multiplayer.cpp:102)). On a *won*
   run, three of the four players would hear the losing sting.
2. `SinglePlayer` has the same shape hard-coded — `winner == 0 ? WIN : LOSE`
   ([single_player.cpp:66](../../src/match/single_player.cpp:66)) — so if the
   AI wingman out-damages you on a cleared run, you lose.

**Recommendation: add one hook to `NetGame`.**

```cpp
// A game the players win or lose TOGETHER. The runners stop asking "was it me?"
// -- everyone hears the same sting, and a win is recorded for every player
// rather than for one. Default false: every game up to Swarm had a winner.
virtual bool teamGame() const { return false; }
```

It is a handful of lines in each runner (`enterResult` and `SinglePlayer::
servicePlaying`) and it makes the standings honest: with `seriesRounds() = 0`
the win-bars then read as **runs cleared together**, which is a genuinely nice
team stat to accumulate over an afternoon.

`isOver()` still reports an MVP by damage dealt, because the border colour and
the framed bar on the result screen want somebody — but nothing punitive hangs
off it any more. On a wipe it reports `NET_PID_NONE` and no win is recorded.

*Alternative, if we want zero runner changes:* MVP-only win recording, and
accept that three players hear a losing sting on a victory. Not recommended —
it is precisely the kind of small wrongness that makes a co-op mode feel like a
PvP mode wearing a hat.

---

## 6. Solo

`supportsSinglePlayer()` returns true and `aiInput()` flies a bot wingman:
track the lowest alien's column, close on it, fire on cooldown, and back off
from a diver.

`SinglePlayer` hard-codes `_numPlayers = 2`
([single_player.cpp:15](../../src/match/single_player.cpp:15)), which happens
to be exactly what Swarm wants solo — you plus one wingman — so **the runner
needs no change**. If we later want three bots, that number should come from
the game rather than being edited in place.

---

## 7. What gets built

```
src/games/swarm/
  swarm_sim.h/.cpp   pure rules, physics, waves, boss — no Arduino, no Display
  swarm.h/.cpp       the NetGame: serialize, render, icon, palette
  swarm_app.h/.cpp   submenu: 1P / MP on stick X, weapon on stick Y
  waves.h            the wave table and boss script, as data
```

Plus:

- `gameId() = 3` (1 = Tron, 2 = Virus).
- Two lines in [main.cpp](../../src/main.cpp): a third `Multiplayer` runner
  bound to Swarm, and a `MENU[]` entry — the same shape as `TronApp` and
  `VirusMenu` already have.
- `+<games/swarm/swarm_sim.cpp>` in the `native` env's `build_src_filter`.
- The `teamGame()` hook and its two runner call-sites (§5).
- 8×8 icon art in `swarm.cpp`, where every other game keeps its icon.

### Tests — `test/test_swarm/`

The sim is pure logic and takes its time as a delta and its randomness as a
seed, so the whole run is assertable off-hardware:

- wave composition is deterministic from the seed — same seed, same swarm
- each weapon's damage rule, including Armour resisting bolts but not blasts
- **no alien is immune to any weapon** (the invariant duplicates depend on)
- no friendly fire: a bullet crossing a friendly ship passes through
- shared-life decrement, respawn timing, invulnerability window
- boss phase transitions and weak-point relocation
- the packed snapshot is 173 bytes, and `serializeState` → `applyState`
  round-trips to an identical view
- a run ends at zero lives and at a dead boss, and at nothing else

---

## 8. Risks and what is deliberately not here

- **Readability is the real risk**, not the netcode. The brightness rule (§1)
  is the mitigation and it should be checked on hardware early — end of Phase
  2, not at the end. If four players plus a full wave reads as noise, the
  answer is fewer, larger aliens, and that is a wave-table edit.
- **A dropped player leaves a ghost ship.** [architecture.md §8](../architecture.md)
  records the general problem: a client's last input stops being replayed after
  500 ms, but the state it already wrote just stands. For Swarm that is a ship
  parked in the band forever. Fixed inside the game — the sim counts ticks
  since each player's last input and parks the ship after ~1 s — so no
  protocol or runner work is needed.
- **Host migration is still out of scope**, unchanged from Tron: if the host
  drops, the run ends.
- **Live loadout visibility in the lobby is deferred.** Showing who has picked
  what before START would need a roster broadcast that does not exist today —
  clients cannot even see each other's *colours* in the lobby at present, only
  the host can. Same gap, worth closing once for both, not now.
- **Per-run high scores** are not in v1. The win-bars carry the team stat and
  `Storage` can gain a "deepest wave" key later without touching anything here.

---

## 9. Phases

Each phase ends somewhere demonstrable, and the first two need no hardware at
all.

| | | Done when |
|---|---|---|
| **0** | Sim skeleton + tests | grid, 4 ships, Blaster, a Grunt wave, collisions — `pio test -e native` green |
| **1** | Weapons + aliens | all 5 weapons, all 4 alien types, the 6-wave table, shared lives, all invariants tested |
| **2** | NetGame + render | pack/unpack with the size assert, the render rules of §1, the HUD row — **first look on hardware** |
| **3** | Wiring | `SwarmApp` submenu, loadout picker, third runner, menu entry, solo + wingman |
| **4** | Boss | body, weak point, phases, volley patterns, victory |
| **5** | Polish | audio (`SFX_HIT`/`SFX_DEATH` exist; Swarm adds a charge and a boss-hit voice), `teamGame()`, tuning with four units on a desk |

Phases 0–1 are where the game is actually designed, and they are testable in
about ten seconds a cycle. Phase 2 is the first moment anything needs to be
flashed.
