# Menu Restructure + User Color — Plan

Status: **implemented** (2026-07-23) — compiles clean for both envs; on-hardware testing pending.
Branch: `test/network`. Clash handling: **next-free-preset** (chosen over MAC-shading).

## Goal

Simplify the shell to **games only** on the main menu, give each game its own submenu, and
replace the A+B "set my number" gesture with an automatic identity + a user-chosen **color**
picked in Settings. No special button combos anywhere.

## Decisions (from Q&A)

| Topic | Decision |
|---|---|
| Back navigation | **Menus only.** B backs out of a submenu; backing out of a game's top submenu returns to the main menu. Launched games/matches run until power-cycle. |
| Enter a game | Any button on the main menu enters the selected game; the game shows its submenu if it has one (not all will). |
| Duplicate colors | **Host auto-varies** clashes to a distinct MAC-seeded shade so all players stay visually distinct. |
| Tetris | Keeps its existing in-game submenu (high score / ghost); just gains "B = back to main menu". |
| Settings | Submenu contains **only User Color** for now. A+B tag editing is removed. |

---

## 1. Navigation model (uses the existing `Game` interface)

No interface change. We already have `GameStatus { GAME_CONTINUE, GAME_EXIT }` — today the shell
ignores `GAME_EXIT`. We start honoring it, and let nested screens signal "back" by returning it.

**Shell (`main.cpp`):**
- `MENU[] = { &tetris, &tronApp, &settings }` (all `Game*`).
- On selection (any button) → `current = game; current->begin(); SYS_GAME`.
- In `SYS_GAME`: `if (current->service() == GAME_EXIT) { SYS_MENU; menu.begin(); }` ← the one new rule.
- Main menu remembers the last-selected page so returning lands where you left.

**Nesting is by return value, one level per screen:**
- A submenu game (TronApp / Settings) manages its own child screens. When it delegates to a child
  (e.g. Multiplayer) and that child returns `GAME_EXIT`, the parent goes back to its submenu.
- When the submenu game itself is backed out of, it returns `GAME_EXIT` and the shell shows the
  main menu.

```
Main menu ──enter──> Tetris ────────────────> (T_MENU: hi/ghost) ──play──> game     B: ->main
          ──enter──> TronApp ──> [Single*]                                           B: ->main
                              └─> [Multiplayer] ──> browse ──> lobby ──> match
                                                     B:->TronApp   B:->browse  (no back in match)
          ──enter──> Settings ─> [User Color] ──save/B──> Settings                   B: ->main
   (* Single Player = placeholder, not implemented this round)
```

---

## 2. Screens to add / change

### TronApp (new `Game`) — `src/tron_app.{h,cpp}`
- Top-level menu entry for Tron; icon = current Tron icon.
- Submenu list: **Single Player** (placeholder), **Multiplayer**. Slider selects, A enters, B → `GAME_EXIT`.
- Single Player → a "coming soon" screen; B back to submenu.
- Multiplayer → runs the existing `Multiplayer` controller (owns the `Tron` NetGame). If it returns
  `GAME_EXIT` (browse backed out), TronApp returns to its submenu.
- Keeps `Multiplayer` generic and reusable for future multiplayer games.

### Multiplayer (`src/multiplayer.cpp`) — changes
- **Remove A+B tag editing** (`MP_SETTAG`, `serviceSetTag`, tag gesture).
- **Browse B = back** → return `GAME_EXIT`. Host/Join is chosen by the slider instead of the B-toggle:
  the slider scrolls a single list `[Host] [join lobby 1] [join lobby 2] …`; A acts on the selection.
- **Browse shows colors, not numbers:** the host entry is your color; each open lobby shows the host's
  color swatch (from `LobbyPayload.colorIndex`).
- **Host lobby** shows a swatch per connected player (from each `JoinPayload.colorIndex`) instead of a
  blinking count.
- Player render colors now come from the **host-resolved palette** (below), not the static Tron palette.

### Settings (new `Game`) — `src/settings.{h,cpp}`
- Submenu list: **User Color** (only entry for now). B → `GAME_EXIT`.
- Color page: slider scrolls the 5 presets; screen fills with the current preset (a swatch) plus a
  small selected indicator. A saves to Storage (`net.setColorIndex`), B cancels. Then back to submenu.

### Tetris (`src/tetris.cpp`) — minimal
- In `T_MENU`, **B returns `GAME_EXIT`** (back to main menu). Everything else unchanged; its existing
  high-score/ghost pages are its submenu.

### Menu (`src/menu.*`) — minor
- Registry becomes the three entries above; remember last page across returns.

---

## 3. Color + identity system

### Presets (shared) — `src/palette.h` (new)
- `PRESET[5] = { cyan, orange, green, magenta, yellow }` (the current Tron player colors).
- One home used by the Settings picker, the lobby swatches, and the games.

### Per-device preference
- `Storage` namespace `"net"`, key `"color"` → `colorIndex` (0..4).
- **Default is automatic:** `colorIndex = mac[5] % 5`, so units differ out of the box with zero user
  action. The user can override it in Settings.
- `Net` gains `myColorIndex()` / `setColorIndex()` (persists), mirroring how the removed tag worked.

### Automatic identity (replaces the A+B number)
- No user-set number. Where a stable per-unit value is needed (dedup seeding), the host uses the
  roster MAC it already stores. Nothing to configure.

### Host-resolved palette (dedup)
- On each JOIN the client sends its `colorIndex`; the host stores it per player.
- At match start the host computes a **resolved CRGB per player** with **next-free-preset dedup**:
  players are resolved in playerId order; each keeps its chosen preset if still free, otherwise it is
  bumped to the next unused preset. With 5 presets and ≤5 players this always yields distinct, clear
  colors.
- The resolved palette is sent in `MSG_START` and passed to the game at `begin()`, so **every screen
  renders every player in the same color**.

### Scoreboard / render
- Games render owner/player colors from the resolved palette (fallback to `PRESET` when solo/no host
  data). `NetGame::playerColor()` reads the resolved palette. The win-bar chart already colors by
  player, so it just uses the new colors.

---

## 4. Wire protocol (bump to **v3**)

| Message | Change |
|---|---|
| `LobbyPayload` | replace `tag` → `colorIndex` (host's color, for the browse swatch) |
| `JoinPayload` | replace `tag` → `colorIndex` (joiner's color) |
| `StartPayload` | add `CRGB colors[numPlayers]` (host-resolved palette) — +3 B/player, still well under 250 |
| `ScoresPayload` | drop `tag`; keep per-player `score` (colors come from the resolved palette everyone has) |
| `NetGame::begin(...)` | add `const CRGB* colors` so the game renders with resolved colors |

All units run the same firmware, so bumping `NET_PROTO_VER` to 3 and reflashing together is the only
compatibility step.

---

## 5. File-by-file

| File | Change |
|---|---|
| `src/main.cpp` | registry = {tetris, tronApp, settings}; honor `GAME_EXIT` → main menu |
| `src/menu.*` | remember last page |
| `src/tron_app.{h,cpp}` | **new** — Tron submenu (Single placeholder / Multiplayer) |
| `src/settings.{h,cpp}` | **new** — Settings submenu + color page |
| `src/palette.h` | **new** — shared 5-color presets |
| `src/multiplayer.*` | remove tag edit; B=back; slider host/join; color swatches; resolved palette |
| `src/net.{h,cpp}` | color pref (get/set/persist); colorIndex in join/lobby; resolve palette; START palette; drop tag |
| `src/net_proto.h` | v3: colorIndex fields, START colors, scores without tag |
| `src/net_game.h` | `begin(...)` takes resolved colors; `playerColor()` reads them |
| `src/tron.{h,cpp}` | consume resolved palette; render from it (keep `PRESET` as fallback) |
| `src/tetris.cpp` | B in `T_MENU` → `GAME_EXIT` |

---

## 6. Suggested build order

1. **Shell back-nav**: honor `GAME_EXIT`; Tetris B→exit. (Small, verifies the nav spine.)
2. **Settings + color page + palette.h + Storage pref** (no networking yet; just persists a color).
3. **TronApp submenu** wrapping the existing Multiplayer; Single Player placeholder.
4. **Color sync + dedup**: wire v3, JOIN/LOBBY colorIndex, resolved palette in START, game renders it.
5. **Multiplayer UI cleanup**: remove tag edit, B=back, slider host/join, lobby swatches.

Each step compiles and is testable; steps 1–3 need no radio, 4–5 use the two-board bench setup.

## 7. Not in scope this round
- Single Player Tron gameplay (placeholder only).
- Other settings (brightness, etc.).
- Back-out of a running game/match (power-cycle as today).
- Client prediction / host migration (still deferred).
