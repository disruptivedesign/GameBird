# Virus multiplayer — bring your own virus

Status: **planned**. Targets the rev 2 board (16×14 Virus arena, 224 tiles) and the existing
host-authoritative net stack. Depends on the per-cell energy model from
[virus-v2-plan.md](virus-v2-plan.md).

## Goal

Four people each write their own `decideA..D` on their own device, walk into a lobby, pick **one** of
their four to enter, and play a best-of-five series on one shared board. Nobody reflashes to change
the roster; nobody needs anybody else's source.

---

## 0. The problem this plan exists to solve

Virus is host-authoritative and the host calls `rule()` once per cell **for every player**. That is
fine locally, where all four rules are in the one binary. It is impossible over the network: the
host does not have the other players' compiled code and never will.

So the thing that crosses the wire is not a rule and not a joystick reading. **It is a decision per
owned cell.** Each tick the host publishes the board; each client runs its own rule over its own
cells and returns a packed list of what those cells want to do; the host validates, funds and commits
exactly as it does today.

This is the first `NetGame` where clients do real work rather than just rendering, which is why the
`NetGame` contract has to grow slightly (§2) — and why `net_game.h`'s "others send inputs + render"
comment stops being true.

### It fits in one frame, which is what makes the whole idea viable

| | bits/cell | worst case (256 cells) | frame budget |
|---|---:|---:|---:|
| Decision (3-bit kind, 2-bit dir) | 5 | **160 B** | 240 B payload |
| Cell energy (0..8, 4 bits) | 4 | **128 B** | 240 B payload |

Worst case is one virus owning every cell, which only happens as the others are wiped out. Four-way,
each player is nearer 56 cells — about 35 B of decisions. There is no pressure here.

> Sized against **256**, a full panel, not the 224 of Virus's HUD-shortened arena: `VIRUS_MAX_CELLS`
> is the panel and the state format is already tested at 16×16. This plan originally said 224 and
> 140 B, and `NET_INPUT_MAX` was set to 144 on that basis. The `static_assert` in `virus.cpp`
> rejected it on the first build. Both are 160 now.

## Decisions

| Topic | Decision |
|---|---|
| Rule execution | **On its author's device.** Decisions cross the wire, tagged with the tick they were computed against. |
| Tick model | **Lockstep with a deadline.** Host publishes the board, collects decisions for it, ticks on its own cadence whether or not they all arrived. |
| Missing / stale decisions | **All Idle.** Covers a dropped frame, a disconnected unit and a rule that ran long, with one rule and no special cases. |
| Session | **Best-of-five, then a champion screen** — same as the local game (`VIRUS_SERIES_ROUNDS`). |
| Fifth unit | **Refused.** `Virus::maxPlayers()` is 4 and `Net::addRoster` already honours it. No work. |
| Rule purity | **Still required.** See §7. |

---

## 1. One tick, over the air

The host already broadcasts a snapshot after every tick. The insight is that **the board after tick
T is the board tick T+1 will read** — so no new publish step is needed, only a relabelling of what
the existing STATE frame means to a client.

```
HOST                                          CLIENT (player p)
  commit tick T
  broadcast STATE  (board, tick=T) ─────────►  applyState()      -- this is the snapshot for T+1
  unicast  PRIVSTATE (p's banks,   ─────────►  applyPrivate()    -- what each of my cells can spend
                      tick=T)
                                               run my rule over my cells
  ◄──────────────────────────────────────────  unicast INPUT (decisions, tick=T)
  ... tick deadline ...
  decisions tagged T?  apply them.
  otherwise             treat as Idle.
  host's own cells: call the local rule directly
  commit tick T+1
```

**Why the tick tag carries the whole design.** A decision list has no cell indices in it — both ends
walk the player's owned cells in scan order over the *same* board and the positions line up. That is
what makes 5 bits per cell enough. It also means a list computed against the wrong board is not
merely stale, it is **misaligned**: entry 7 would land on somebody else's cell. So the host accepts a
list only if its tag matches the tick it is about to simulate, and otherwise takes the Idle path.
One check, and every failure mode — dropped STATE, dropped INPUT, slow rule, dead unit — degrades to
the same safe behaviour.

**Cheating is structurally hard, for free.** A decision list is positionally indexed by *your own*
cells, so it has no field in which to name somebody else's. Beyond that the referee still runs
`applyIfLegal` and still funds from the cell's own bank, so a doctored client can at best make its
own cells act badly.

## 2. Protocol and `NetGame` changes

Small, and mostly generic rather than Virus-shaped.

| Change | Why |
|---|---|
| `NET_PROTO_VER` 4 → **5** | New message type, and `tick` becomes meaningful on INPUT. A v4 unit must not half-understand this. |
| `NET_INPUT_MAX` 16 → **144** | Holds a worst-case decision list. Costs 5 × 144 B of roster RAM. |
| **`MSG_PRIVSTATE`** (new) | Host → *one* client, game-opaque. Per-player private state. Virus uses it for cell energy; it is the obvious channel for any later game with a hand, a fog of war, or anything else not everyone may see. |
| `NET_PRIV_MAX` **128** | Payload cap for the above. |
| `NetGame::serializePrivate(pid, buf, cap)` | Host builds player `pid`'s private view. Default returns 0, so Tron is untouched. |
| `NetGame::applyPrivate(buf, len)` | Client absorbs it. Default no-op. |
| `NetGame::lockstep()` | `false` by default. Tells the runner to gate the tick on decisions instead of streaming inputs. |
| `NetGame::seriesRounds()` | `0` = endless rematch (Tron today). Virus returns 5. |

`serializeInput`/`applyInput` need no change at all: Virus simply ignores the `LocalInput` it is
handed and emits its decision list, and `applyInput(pid, …)` stores a remote player's list rather
than a heading. That the existing interface absorbs this is a good sign the split was drawn in the
right place.

## 3. Referee changes (`Virus`)

The sim keeps one board and one rule table; what changes is **where a player's decision comes from**.

- `_rule[p]` set → call it, as now. This is the local player on any device, and every player in a
  single-device game, so the local game is unaffected.
- `_rule[p]` null → consume `_pending[p]`, the decoded list from the wire. Absent or mis-tagged →
  Idle.

New surface:

- `decideLocal(buf, cap)` — walk my own cells against the last applied board and banks, call my rule,
  pack. This is what a client runs; it is the same loop as `hostTick`'s decide phase, minus the
  funding and commit.
- `packDecisions` / `unpackDecisions`, `packBanks` / `unpackBanks` — the codecs, with the owned-cell
  scan order in exactly one place so the two ends cannot drift.
- `setNetRole(myId)` — marks which slot is locally ruled.

**The energy wrinkle.** [virus-v2-plan.md](virus-v2-plan.md) made `_cellBank` host-only and not
serialized, on the stated reasoning that "clients render, they never simulate." This feature is
precisely the thing that breaks that assumption, and `me.canAfford()` — which we deliberately
promoted from advisory to binding — is unusable without it. Hence `MSG_PRIVSTATE`. The alternative,
having each client track its own banks, was rejected: banks travel with a Move, reset on death and
on a new cell, and a contested move fails, so a client would be re-deriving host state from
inference and would drift the first time it guessed wrong.

## 4. Runner

Extend `Multiplayer` rather than writing a second one. The lobby, browse, roster, colour resolution,
countdown and scoreboard are all already there and all apply unchanged; the differences are confined
to two places:

- `servicePlayingLockstep(now)` — the loop in §1, selected by `_game->lockstep()`. The existing
  async path stays exactly as it is for Tron.
- Series — after each match, `matchesPlayed++`; at `seriesRounds()` show final standings with the
  champion framed, mirroring `VirusApp::serviceSeriesOver`. `seriesRounds() == 0` keeps Tron's
  endless rematch.

## 5. Picking your virus

A Virus submenu, the same shape as `TronApp`: **Single** (today's local series) / **Multiplayer**.
Choosing Multiplayer shows a one-of-four picker — A/B/C/D in your device colour, stick to move, A to
confirm — which sets that rule as the local player's, then hands off to `Multiplayer`.

Letters are local: my A and your A are different code, so **a player is identified on screen by
their colour**, exactly as in Tron. The letter is only ever shown on your own device, and in your own
serial telemetry.

## 6. Tests

All of this is serialization and referee logic, so it tests natively with no radio:

- decision list round-trips, every action kind and direction
- host and client derive the identical owned-cell order from the same board
- a list tagged with the wrong tick is ignored, and that player idles
- an absent list idles that player and does not disturb the others
- bank list round-trips, including the 0 and 8 ends of the 4-bit range
- worst case — one virus owning all 224 cells — fits `NET_INPUT_MAX` and one ESP-NOW frame
- a single-device match still plays identically (guards the local path)

The last one matters most: the local game is the thing people already use, and it is the easiest
casualty of this change.

## 7. Rule purity stays required

With rules running on their author's hardware and only decisions crossing the wire, a `static` would
no longer break anything *in multiplayer* — the host never replays anyone else's rule. It would still
break the local game, which replays rules over a snapshot to stay reproducible from `(seed, roster)`,
and the native tests, which compile `virus_rules.cpp` off-hardware.

Keeping one contract. "Pure here, not pure there" is the kind of rule people get wrong once and then
cannot debug, and the payoff — per-tick memory — is a feature nobody has asked for.

## 8. Build order

1. ✅ Protocol: version bump, `MSG_PRIVSTATE`, `NET_INPUT_MAX`, the four `NetGame` hooks. Tron still builds and plays.
2. ✅ Codecs + `decideLocal` + pending-list plumbing in `Virus`, with the §6 tests. No networking yet.
3. ✅ **Folded into 2.** The energy codec cannot be deferred: without it a client's cells all read
   zero energy, so nothing they decide is testable end to end. Both codecs landed together.
4. ✅ `Multiplayer` lockstep loop. Needed one thing this plan missed: `NetGame::tickMs()`. Virus ticks
   at 180 ms and the shared match cadence is 40 ms, so without it a networked match would run four
   and a half times too fast *and* put a lockstep round trip on the wire 25 times a second.
5. ✅ Series handling and the champion screen. Also replaced the missed-START detector: it keyed off
   the lobby beacon's `started` flag, which cannot tell "started without me" from "has not reopened
   since the last match", and so needed an arming step. Between series matches the host never reopens
   at all, so that arming would never have fired — the protection would have been silently absent for
   exactly the case it was written for. A snapshot arriving while still in the lobby says it without
   ambiguity.
6. ✅ Virus submenu + one-of-four picker; registered in `main.cpp`. Needed `Virus::setLocalRule()`:
   the player picks a virus *before* joining, so the slot it belongs in does not exist yet and
   `begin()` has to place it once the lobby assigns an id.
7. On hardware: two units, then four.

### Review before hardware

A read-through once everything was in place turned up three things worth fixing first, all of them
invisible to the native tests because they live where the game meets a runner:

- **The HUD had no owner in multiplayer.** `drawHud()` lived in `VirusApp`, so a networked match had
  two dead rows — and `cellCount()` is filled by `evaluateEnd()`, which a client never runs, so the
  territory bar would have been blank on a client even if drawn. The HUD moved into
  `Virus::render()`, where it belongs (the game is what shortened its own arena for it), and
  `applyState()` recounts.
- **A client lingered on the result screen after the host had moved on.** The host only has to wait
  out the 500 ms lockout, so a press there sends START while the client is still counting out three
  seconds — and by the time it joined, the host had been ticking for over a second with that virus
  idle. Clients now take START straight from the result screen.
- **Telemetry could not tell a quiet virus from an absent one.** `act=x/y` looks identical whether a
  rule chose to idle or its device never answered, which are completely different problems on a
  bench. Each player now carries `.` local, `+` decisions arrived, `-` nothing heard.

Known and deliberately left:

- **Networked Virus is silent.** Voices are `VirusApp`-only and `_ev[]` is host-only. Wiring them up
  means either a `NetGame` audio hook or moving the voice layer into the game.
- **`INPUT_TIMEOUT` (500 ms) couples to the tick period.** Virus ticks at 180 ms so replies are used
  well inside it, but raise `VIRUS_TICK_MS` past 500 and every decision list expires before use — the
  match would look like all four players had dropped at once.
- **No damage or kill flashes on a client.** `_fx` is written in `commitTick()`. Harmless as it
  stands (`_fx` is zero-initialised and `_tickWallMs` stays 0, so the fade amount is always 0).

Steps 1–3 are the bulk and are all natively testable. Nothing before step 7 needs a radio.

## 9. Deliberately not in this

- **Host migration.** The host drops, the match dies, as everywhere else in the stack.
- **Spectating.** A fifth unit is refused from a full lobby, per §Decisions.
- **Shipping rules over the air.** The reason this design exists.
- **A rule time budget.** A pathological rule blows its own deadline and idles itself; it cannot
  stall the match, so a budget would be enforcement without a victim.
- **Making a disconnected virus die.** It idles and keeps its ground, so rivals still have to fight
  through it. Killing it would dump a large field of free neutral territory into a live match and
  swing the result more than the disconnect did.
