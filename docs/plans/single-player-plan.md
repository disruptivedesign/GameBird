# Single-Player Tron (vs AI) — Plan

Status: **implemented** (2026-07-23) — compiles clean for both envs; on-hardware testing pending.
Branch: `test/network`.

## Goal

Add a single-player Tron mode that reuses the **exact same `Tron` simulation** as multiplayer, with
one AI opponent. Because all gameplay lives in `Tron`, any tweak or bugfix there applies to both modes
by construction.

## Decisions (from Q&A)

| Topic | Decision |
|---|---|
| Reuse model | **Shared sim + thin, game-agnostic 1P runner.** No changes to the multiplayer control flow → no regression risk. |
| AI difficulty | **One tuned difficulty** now; a selector can be added later without rework. |
| 1P rounds | **Running tally vs AI**, shown with the same win-bar scoreboard as multiplayer; auto-rematch; B exits. |
| Players | 1 human (player 0) + 1 AI (player 1) = 2. Structured to allow more AI later. |

---

## 1. Architecture

Single-player is simply a **local, network-free host match**. The host-authoritative loop already
separates "produce input" from "advance sim", and the sim consumes input for any player via
`applyInput()` — the AI just fills that same buffer for the opponent.

```
SinglePlayer (new, game-agnostic Game)         Tron (unchanged sim)
  each tick:
    human  -> serializeInput(local)  --------> applyInput(0)
    AI     -> aiInput(1)             --------> applyInput(1)
    hostTick()                       --------> (same collision/trail/win logic as MP)
    render(disp)                     --------> (same renderer as MP)
```

- **No new sim code.** `Tron::begin/applyInput/hostTick/serializeState/isOver/render/playerColor` are
  used verbatim. Steering, speed, trail growth, collisions — all shared.
- The runner is **game-agnostic** (takes a `NetGame*`), so future single-player-capable games reuse it.

## 2. AI — lives on the game (via the `NetGame` interface)

The AI needs the board, so it belongs with the game. Add to `NetGame`:

```cpp
// Default: go straight, no boost. Games that support 1P override it.
virtual size_t aiInput(uint8_t playerId, uint8_t* buf, size_t cap);
virtual bool   supportsSinglePlayer() const { return false; }   // Tron: true
```

**`Tron::aiInput` (one tuned difficulty):** greedy open-space avoidance.
- Candidate headings: straight, left, right (never reverse — that's instant self-collision).
- Score each by a small **flood-fill of reachable empty cells** from the cell it would move into
  (cheap on 8×8/16×16 at ~2.5 cells/s). A candidate that would hit a wall/trail scores as death.
- Pick the highest-scoring heading (ties prefer straight, so it doesn't twitch); emit the `steer`
  byte that turns toward it — identical wire format to `serializeInput`, so `applyInput` is unchanged.
- Boost off for v1.
- This avoids walls/trails and escapes pockets, so it's a real opponent but beatable.

**Difficulty knobs for later** (not built now): flood-fill depth cap, an N%-of-the-time random
mistake, reaction lag, and boost usage. These map cleanly onto the same method.

## 3. Menu integration

`TronApp`'s existing `SINGLE` state stops being a placeholder and delegates to the `SinglePlayer`
runner, exactly mirroring how `MULTI` delegates to `Multiplayer`:

```cpp
case SINGLE:
  if (_sp.service() == GAME_EXIT){ _state = SUB; _sel = 0; }
  break;
```

`TronApp` gains a `SinglePlayer&` (constructed in `main.cpp`, sharing the one `Tron` instance with
`Multiplayer` — they never run at the same time).

## 4. Runner states & scoring

`SinglePlayer` states: `SP_COUNTDOWN → SP_PLAYING → SP_RESULT` (then auto-rematch).
- **begin():** colors[0] = your chosen color (`net.myColorIndex`), colors[1] = a distinct preset;
  `game.begin(MATRIX_W, MATRIX_H, myId=0, numPlayers=2, seed, colors)`; reset the win tally.
- **playing:** the loop in §1 at the shared `HOST_TICK_MS`. `B` exits to the submenu at any time
  (B is unused by Tron gameplay).
- **result:** on `isOver`, bump the local tally for the winner (0/1, or neither on a draw), show the
  final board briefly then the win-bar scoreboard, and rematch after the timeout.

## 5. Shared scoreboard helper (small refactor, aids the reuse goal)

Extract the multiplayer win-bar renderer into a free function so both modes render standings
identically (and future tweaks hit both):

```cpp
// src/scoreboard.{h,cpp}
void drawWinBars(Display& d, const uint8_t* scores, const CRGB* colors,
                 int n, int winner, uint32_t now);
```

`Multiplayer::drawScoreboard` becomes a thin call (net scores + `game.playerColor`); `SinglePlayer`
calls it with its local tally. The countdown render is trivial and may be shared the same way or left
duplicated.

---

## 6. File-by-file

| File | Change |
|---|---|
| `src/net_game.h` | add `aiInput(...)` (default: straight) and `supportsSinglePlayer()` (default false) |
| `src/tron.{h,cpp}` | implement `aiInput` (flood-fill avoidance) + `supportsSinglePlayer()=true`; expose small state helpers if needed |
| `src/single_player.{h,cpp}` | **new** — game-agnostic 1P runner (countdown/play/result, local tally, AI) |
| `src/scoreboard.{h,cpp}` | **new** — shared win-bar renderer |
| `src/multiplayer.cpp` | `drawScoreboard` calls the shared helper (no behavior change) |
| `src/tron_app.{h,cpp}` | `SINGLE` delegates to `SinglePlayer`; gains a `SinglePlayer&` |
| `src/main.cpp` | construct `SinglePlayer sp(sys, &tron)`; pass into `TronApp` |

No wire-protocol change (single-player is offline). Multiplayer control flow untouched.

## 7. Build order

1. `NetGame::aiInput` default + `Tron::aiInput` (flood-fill). Unit-check the AI by eye via 1P once wired.
2. `SinglePlayer` runner (countdown/play/result + tally), colors from preferences.
3. Extract `scoreboard.{h,cpp}`; point Multiplayer and SinglePlayer at it.
4. Wire `TronApp` SINGLE → `SinglePlayer`; `main.cpp` construction.

Steps 1–2 are independently testable on a single unit (no radio needed) — this is the mode that
finally makes the screen-only board fully playable on its own.

## 8. Not in scope this round
- Difficulty selector / multiple AI opponents (architecture leaves room).
- AI boost usage.
- Single-player for other games (interface is ready; only Tron overrides `aiInput`).
