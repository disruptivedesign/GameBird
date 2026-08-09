# Wireless Multiplayer — Architecture

**This is the living architecture reference — keep it current when the network stack
or the source layout changes.** Completed design records live in [`docs/plans/`](plans/);
they are point-in-time and are not maintained.

Status: **v1 implemented**, both envs build clean — **still not run on hardware**, and the net layer
has no automated tests, so everything below is reviewed rather than demonstrated.
Target: 4–5 GAMEBIRD units in the same room.

**Networked today: Tron, Virus and Swarm.** `main.cpp` constructs one `Multiplayer` runner per
networked game rather than one shared — a runner is built around the game it drives, and only one
can hold the radio at a time anyway.

**Swarm is the first co-op game**, and it is the one that showed where the stack assumed a single
winner. Its per-player weapon rides in the opaque INPUT blob rather than in `JoinPayload`, so the
match data plane did not move at all. What it did add is `NetGame::teamGame()` (Layer 4 below) and,
at **protocol v6**, a lobby roster every unit can see — the host used to be the only device that
knew who was in the room.

Sections 1–5 describe how the system works today. Sections 6–9 are the original
vertical-slice plan and phase log, kept for the rationale behind the design.

## Source map (implemented)

| Layer | Files |
|---|---|
| Protocol (constants + wire structs) | `src/net/net_proto.h` |
| 1. Transport (ESP-NOW + RX queue) | `src/net/net_link.h/.cpp` |
| 2/3. Session/Lobby + Message/Sync | `src/net/net.h/.cpp` (owned by `System`) |
| 4. Game interface | `src/net/net_game.h` (`NetGame`, `LocalInput`) |
| Lobby UI + match runner | `src/match/multiplayer.h/.cpp` (a menu `Game`) |
| Single-player runner (vs AI) | `src/match/single_player.h/.cpp` |
| Shared match cadence + countdown/result shell | `src/match/match_config.h`, `src/match/match_shell.h/.cpp` |
| Vertical slice game | `src/games/tron/tron.h/.cpp` + `tron_app.*` (submenu) |
| Co-op wave shooter | `src/games/swarm/` — `swarm_sim.*` (rules, native-tested), `swarm.*` (NetGame + render), `waves.h`, `swarm_app.*` (submenu + loadout picker) |
| Programmable cellular game | `src/games/virus/` — `virus.*` (referee), `virus_api.h`, `virus_rules.cpp` (author-editable), `virus_app.*` |
| Audio (piezo service + catalog) | `src/audio/` — `sound.h` (POD types), `notes.h`, `audio.*` (LEDC + arbiter), `sfx.*` (shared catalog) |
| Wiring | `src/core/system.*` (adds `Net`, `Audio`), `src/main.cpp` (menu entry) |
| Native tests (no hardware) | `test/test_tron/`, `test/test_virus/`, `test/test_swarm/`, shims in `test/shims/` |

## Decisions locked in

| Question | Decision |
|---|---|
| Authoritative simulation | **Host-authoritative** — one unit per match runs the sim; others send inputs + render |
| Timing profile to support | **Real-time *and* turn-based** (one backend, game picks its cadence) |
| Pairing UX | **Auto-discover + lobby**, a game-agnostic **main-menu page** — no codes, no pre-pairing |
| Game selection | Host picks the **game type** in the lobby; carried as `gameId` in lobby/START |
| Display sizes to support | **8×8 today, 16×16 on next hardware** — backend is resolution-agnostic *now* |
| First deliverable | **Vertical slice** — Net service + a working 2D Tron together |
| Transport (recommended) | **ESP-NOW** — see rationale below |

---

## 1. Hardware reality that drives everything

- **MCU:** ESP32-S3FH4R2 (dual-core 240 MHz, 2 MB PSRAM). Native **2.4 GHz radio doing WiFi + BLE 5.0** — no add-on hardware for wireless.
- **Display:** **16×16 WS2812B (256 px)** on rev 2. (Rev 1 was 8×8 SK6812 RGBW; the tree stayed resolution-agnostic and the native tests still exercise both sizes.)
- **Controls:** 2 buttons (A/B) + a **2-axis analog joystick** (`src/core/controls.h`). Rev 1 had a pot slider. The joystick both adds an axis and changes the *kind* of control — a pot holds its position, a stick springs back — which is why every selector on the device now steps relatively instead of mapping position to index.

**Two consequences:**

1. **Small display ⇒ no shared screen.** Every unit renders its *own* identical view of one shared world state. For Tron, each device draws the full arena from the common state. So "master vs. independent" resolves to: **host owns the sim, everyone renders the same broadcast state.**
2. **Two display generations ⇒ resolution-agnostic backend.** All coordinates, state sizing, and the game's grid derive from `MATRIX_W`/`MATRIX_H` (`src/core/defines.h`). The netcode carries **arena dimensions in the START message** so a session is self-describing. On next-gen hardware you bump `MATRIX_W/H` to 16 and the game + serialization scale automatically; the rest of the system is *not* touched until then. (Mixed-generation matches — an 8×8 unit vs. a 16×16 unit — are out of scope for v1 but detectable via the arena dims, so they fail loudly rather than corrupt state.)

**Payload sizing across resolutions.** ESP-NOW caps a frame at 250 B and `NetHeader` is 10 B, so a STATE payload has **240 B** to work with (`NET_STATE_MAX`).

*Tron* — nibble-packed owner map plus per-player heads:

| State element | 8×8 | 16×16 |
|---|---|---|
| Per-cell owner map @ 4 bits/cell | 32 B | 128 B |
| Header + per-player heads (`x,y,dir|alive`) ×5 | ~18 B | ~18 B |
| **Total STATE snapshot** | ~50 B | ~146 B |

*Virus* — 6 bits per cell (3-bit owner, 3-bit strength), four cells per three bytes:

| State element | 8×8 | 16×16 |
|---|---|---|
| Per-cell owner + strength @ 6 bits/cell | 48 B | 192 B |
| Header (`phase, winner, numPlayers, arenaW, arenaH`) | 5 B | 5 B |
| **Total STATE snapshot** | 53 B | **197 B** |

*Swarm* — an entity list rather than a cell map, because most of a shooter's screen is empty:

| State element | 16×16 |
|---|---|
| Header (`phase, wave, lives, numPlayers, bossHp` + three entity counts) | 8 B |
| Players `x, y, weapon/alive/meter, invuln/shield` ×4 | 16 B |
| Aliens `x, y, type/hp/flags` ×28 | 84 B |
| Bullets `x, y, kind/owner/dir` ×20 | 60 B |
| Effects `cell, kind` ×6 | 12 B |
| **Total STATE snapshot (worst case)** | **180 B** |

Positions are Q4.4, which is one byte per axis on a panel up to 16 wide — so the sub-cell precision
a shooter needs costs nothing over whole cells. The frame is variable length: only live entities are
sent, so a quiet screen is a short frame and 180 B is the ceiling `test_swarm` pins.

All three fit a single frame at both resolutions, so the full-snapshot-every-tick model holds. Virus is the binding constraint: the obvious byte-per-cell layout would be 261 B at 16×16, over the wire cap outright, which is why its owner code is capped at 3 bits (`OWNER_NEUTRAL` is 6, not a nibble's 15) and why `NET_STATE_MAX` is 240 rather than 200. `test_virus` asserts the 197-byte figure so the budget cannot drift unnoticed.

---

## 2. Transport: why ESP-NOW

| Option | Latency | Needs router/AP? | Peers | Verdict |
|---|---|---|---|---|
| **ESP-NOW** | ~1–2 ms | **No** | ~20 | **Chosen** |
| BLE 5.0 | ~7.5–30 ms | No | ~7 practical | Higher latency, fiddly GATT roles |
| WiFi (SoftAP / Direct) | ~5–50 ms | Association + DHCP | many | Heavier; join step; a unit becomes an AP |
| WiFi mesh (ESP-MESH) | varies | No (self-forms) | many | Overkill for one room, 5 units |

**ESP-NOW** rides the WiFi radio but is *connectionless* — no SSID, no router, no DHCP, no "join the WiFi." It gives broadcast (discovery) + unicast (MAC-layer ACK), 250-byte payloads, a fixed 2.4 GHz channel we set ourselves, and MAC-addressed peers. It directly satisfies the "don't connect to an existing WiFi network" constraint. The transport is isolated behind an interface (§3) so BLE could swap in later without touching game or lobby code.

**Coexistence caveat:** if a unit ever *also* joins a real AP for internet, ESP-NOW must use that AP's channel. Keep radio ownership in one place so this stays a one-line change.

---

## 3. Layered architecture (the agnostic backend)

Games never see `esp_now.h`. Four layers, each swappable:

```
┌─────────────────────────────────────────────────────────┐
│ 4. Game integration   NetGame interface: gameId,         │
│                       serializeState/applyState,         │
│                       serializeInput/applyInput, hostTick│
├─────────────────────────────────────────────────────────┤
│ 3. Message / Sync     typed frames (header + payload):   │
│                       BEACON, LOBBY, START, INPUT, STATE,│
│                       EVENT, LEAVE. Opaque game blobs.   │
├─────────────────────────────────────────────────────────┤
│ 2. Session / Lobby    identity, peer table, discovery,   │
│    (game-agnostic,    game-type pick, host/join, roster  │
│     its own menu page)                                   │
├─────────────────────────────────────────────────────────┤
│ 1. Transport          ESP-NOW init, channel, send        │
│    (only ESP-NOW here) (bcast/unicast), RX → queue       │
└─────────────────────────────────────────────────────────┘
```

Fits the existing service pattern — `System` already owns `Display`, `Storage`, `Menu` (`src/main.cpp`). Add a **`Net` service** the same way.

### Layer 1 — Transport (`src/net/net_link.*`)
- `begin(channel)`: WiFi in STA mode, **not connected**; `esp_now_init()`; add broadcast peer `FF:FF:FF:FF:FF:FF`; register RX callback.
- `sendBroadcast(buf,len)`, `sendTo(mac,buf,len)`.
- **RX callback runs in the WiFi task** — it must only copy the frame into a fixed-size **FreeRTOS queue / ring buffer** and return. The main loop drains it each tick. (Doing work in the callback is the classic ESP-NOW footgun.)

### Layer 2 — Session / Lobby (`NetSession`) — a game-agnostic main-menu page
- **Entry point:** a new **"Multiplayer" page** off the main menu (`src/ui/menu.*`), independent of any game. Opening it starts discovery.
- **Identity:** device MAC read at boot; last 3 bytes → short id + default name (rename later, persist via `Storage`).
- **Discovery:** broadcast a **presence beacon** every ~250 ms: `{deviceId, name, status, hostingGameId?}`. Peers build a table, pruning entries not heard from in ~1 s.
- **Host + game-type pick:** "Host a game" → host chooses which **NetGame** (from a registry) → broadcast a **lobby announce** `{sessionId, gameId, arenaW, arenaH, hostMac, players[]}`.
- **Join:** others list open lobbies (with their game type), pick one → unicast `JOIN` → host assigns **playerId (0..N-1)** and broadcasts the updated roster.
- **Start:** host broadcasts `START` (gameId, arena dims, RNG seed, spawn slots) + short countdown; everyone transitions to the game.
- **Leave / host loss:** on `LEAVE` or host-beacon timeout, others return to the lobby ("host left"). *True host migration is deferred to v2 (§8).*

### Layer 3 — Message / Sync
Fixed header (~10 bytes) on every frame:

```
magic(2) | protoVer(1) | msgType(1) | gameId(1) | sessionId(2) | senderId(1) | tick/seq(2)
```

- `magic` + `protoVer` + `gameId` fence off stray/other-project traffic and version skew.
- **INPUT** (client→host) and **STATE** (host→all) carry an **opaque game blob** — the backend never parses it; the game owns its layout.
- Lobby/EVENT messages are structured and backend-owned.
- **Clock:** host's `tick` number travels in every STATE frame; clients follow the host clock. No NTP, no lockstep.

### Layer 4 — Game integration (`NetGame`)
Extends the existing `Game` (`src/core/game.h`) without disturbing single-player games (Tetris stays untouched):

```cpp
class NetGame : public Game {
public:
  virtual uint8_t gameId()     const = 0;   // stable id; only matching games talk
  virtual uint8_t maxPlayers() const = 0;

  virtual void   begin(uint8_t arenaW, uint8_t arenaH,   // sized from START, not
                       uint8_t myPlayerId, uint32_t seed) = 0;  // hard-coded 8×8

  // Client → produce local input each tick (steer intent + boost), tiny.
  virtual size_t serializeInput(uint8_t* buf, size_t cap) = 0;

  // Host → consume one player's input, advance sim, produce state.
  virtual void   applyInput(uint8_t playerId, const uint8_t* buf, size_t len) = 0;
  virtual void   hostTick() = 0;                       // authoritative sim step
  virtual size_t serializeState(uint8_t* buf, size_t cap) = 0;

  // Client → adopt authoritative state; render happens in service().
  virtual void   applyState(const uint8_t* buf, size_t len) = 0;
};
```

The Net service drives the loop; the game only fills/reads buffers and renders. Because `begin()` receives arena dims, the same game binary runs on 8×8 and 16×16.

**`teamGame()`** (default false) is the one addition co-op forced. The runners were built around a
single winner: they compare the winner's id against your own to pick the closing sting, and credit
the win to that player. Neither question has an answer in a game the squad wins or loses together —
left alone, three of four players would hear the *losing* sting on a cleared run. Told true, a
runner plays the same sting everywhere and calls `Net::recordTeamWin()`, so the standings read as
runs cleared together. `isOver()` still names somebody, because the result screen wants a border
colour and a framed bar, but nothing punitive hangs off it; Swarm reports its MVP by damage dealt on
a win and `NET_PID_NONE` on a wipe.

---

## 4. Netcode model (host-authoritative, real-time)

Fixed tick on the host (Tron ~15 Hz; turn-based games tick on events instead).

**Each host tick:** gather inputs (own + latest from each client) → `hostTick()` advances sim, resolves collisions/scoring → `serializeState()` → **broadcast STATE**.

**Each client tick:** sample local input → `serializeInput()` → **unicast INPUT** to host → render the most recent applied STATE.

**Deliberate v1 simplifications:**
- **No prediction/rollback** — at ~2 ms transport and 15 Hz ticks, perceived lag is ~one frame. Client prediction is a clean later add if a faster game needs it.
- **Full snapshot every tick, no deltas/acks** — state is tiny (§1); a dropped frame self-heals on the next one 66 ms later.

**Bandwidth check (5 units, 15 Hz, 16×16):** host sends 15 × ~110 B/s; each client sends 15 × ~5 B/s. Far under ESP-NOW capacity.

---

## 5. What crosses the network

| Message | Dir | Rate | ~Size | Payload |
|---|---|---|---|---|
| BEACON (presence) | bcast | 4 Hz | ~20 B | deviceId, name, status, hostingGameId |
| LOBBY announce/roster | bcast | 4 Hz | 17 B | gameId, arenaW/H, playerCount, maxPlayers, started, **slot mask**, and the roster: `colorIndex[5]` + `tag[5]` |
| JOIN / LEAVE | unicast | on event | ~14 B | deviceId, colorIndex, tag |
| START | bcast | once | ~20 B | gameId, arena dims, seed, spawn slots, countdown |
| **INPUT** | client→host | tick | ~4 B, up to 144 B | steer intent (opaque); a lockstep game sends a whole decision list instead, tagged with the tick it was computed against |
| **STATE** | host→bcast | tick | ~40–197 B | game snapshot (opaque) |
| **PRIVSTATE** | host→one client | tick, lockstep only | up to 128 B | that player's private state (opaque) — Virus sends each client the energy its own cells have banked |
| SCORES | host→bcast | on win / on join | ~6 B | per-player win counts (colors come from START) |

**The roster rides the beacon** rather than getting a message of its own. Before protocol v6 a
client could not see who else was in the lobby *at all* — not their colours, not anything — because
only the host held the roster and nothing carried it. The host drew colour swatches and a client
drew a spinner. Putting it in a frame that already goes out 4 Hz and already carries the player
count costs 10 bytes on a frame with 200 spare, and it means the count and the roster cannot
disagree; a dedicated "roster changed" message would have added a frame whose loss leaves a client
showing a lobby that no longer exists.

`tag[]` is **one opaque byte per player** — the same opacity INPUT and STATE have, applied to the
lobby. The backend carries it and never reads it; the game says what it means via
`NetGame::lobbyTag()` and draws it via `NetGame::drawLobbyTag()`. Swarm puts each player's chosen
weapon there, so a squad can see it is about to go in with four Bombs and re-pick first.

`slots` is a **bitmask, not a count**, and anything drawing a roster must iterate it. `playerCount`
is a count, and a departure in the middle leaves a hole — two players in slots `{0,2}` — so the old
swatch strip walking `0..playerCount-1` drew an empty seat and missed a real player.

Payload budgets are `static_assert`ed against the 250-byte ESP-NOW cap in `net.cpp`, along with the
10-byte `NetHeader` every one of them was derived from.

### Lockstep games (protocol v5)

Every game up to Tron sends a control reading and lets the host simulate. Virus cannot: each
player's rule is compiled into their own device, so the host has no way to execute it. Such a game
sets `NetGame::lockstep()`, and the exchange inverts — the host publishes a board, each client runs
its own rule over its own cells and returns a decision per cell, and the host validates and commits
those exactly as if it had produced them.

The tick tag on INPUT is what makes it safe. A decision list carries no cell indices: both ends walk
the sender's owned cells in scan order over the same board, which is what gets it down to five bits
per cell. So a list applied to a board it was not computed against is not stale, it is *misaligned*.
The host matches the tag and treats any mismatch as "this player did nothing" — the same path a
dropped frame, an overrunning rule and a departed unit all take. Full design:
[virus-multiplayer-plan.md](plans/virus-multiplayer-plan.md).

---

## 6. Vertical slice — 2D Tron *(mechanics finalized)*

- **Arena:** the full grid, sized from `begin()`. 2–5 players, each an always-moving point that lays a **finite trail**. Speed, starting trail length and growth rate are **derived from the arena**, so four times the ground still plays at a comparable pace.
- **Movement:** **always forward** (classic Tron) — a player never stops and continuously extends its trail.
- **Controls:** **stick = absolute heading.** Byte 0 of the input blob carries a heading (0–3) or `TRON_NO_TURN`; diagonals resolve to the dominant axis and an exact tie requests nothing. **Reversals are rejected in the sim**, not the UI — a reversal is instant self-collision and the client is not trusted to have checked. **A = speed boost** while held. **B** free (lobby "ready" / pause). *(Rev 1 sent a relative −1/0/+1 turn in the same byte. Same size, different meaning, so `NET_PROTO_VER` went to 4 — the version byte is the only thing that makes a mixed fleet fail loudly instead of steering itself into a wall.)*
- **Boost:** held A drives faster (e.g. 2 cells/tick or a shorter per-player move interval). *Default (tunable): drains a small rechargeable stamina meter so it can't be held forever;* set stamina to infinite if we want it dead-simple for v1.
- **Trail:** each player's trail has a **maximum length** (a short ring buffer of cells); as the head advances past that length the **tail cell clears**. Max length **grows over time** — *default (tunable): start ~4 cells, +1 every ~5 s* — so the arena fills and forces conflict (anti-stalemate).
- **Death:** a head entering a cell occupied by **any trail — including its own — or the arena edge (wall = death)** → **explode**. **Last player alive wins.** Two heads into the same cell in one tick → both explode.
- **Authoritative state:** per-cell **owner map** (0 = empty, 1..N = player color) + per-player `{headX, headY, heading, alive, colorId}`. Host also keeps each player's trail ring buffer to expire tails. Clients render the owner map directly, so all screens match.
- **Start:** host sends spawn slots + seed in START so everyone begins identical.

**Note on 8×8 today:** with growing trails, 8×8 comfortably fits ~2–3 players; the full 4–5 is a better fit once 16×16 hardware lands. The game scales automatically either way.

---

## 7. Integration points in the current code

- `src/ui/menu.*` / `src/main.cpp` — add a **"Multiplayer" page** to the main menu that opens the game-agnostic lobby; single-player games keep their existing registry path untouched.
- **NetGame registry** — a table of available networked games (like the current `MENU[]`) the host picks from in the lobby.
- `System` (`src/core/system.*`) — own a `Net` service beside `Display`/`Storage`.
- `src/core/game.h` — add the `NetGame` subclass; `Game`/Tetris unchanged.
- `Storage` (`src/core/storage.*`) — persist device name (and later, stats).
- `src/core/defines.h` — add `NET_CHANNEL`, `NET_MAGIC`, `PROTO_VER`, `MAX_PLAYERS`; `MATRIX_W/H` already the single source of truth for resolution.

---

## 8. Risks & deferred work

- **RX callback context** — copy-to-queue only (§Layer 1). *Primary correctness risk.*
- **Channel agreement** — all units on one fixed channel; revisit only if a unit also joins an AP (§2).
- **Mixed display generations** — 8×8 vs 16×16 in one match is out of scope for v1 but detected via START arena dims (fails loudly, not silently).
- **Host migration** — v1 ends the match if the host drops; v2 can elect a new host + transfer state.
- **Security** — the `magic`/`protoVer` fence keeps other projects/stray traffic out (the practical concern in a shared space). True ESP-NOW encryption was **consciously de-scoped**: ESP-NOW cannot encrypt broadcast frames, and our discovery + per-tick STATE fan-out are broadcast, so PMK/LMK would only protect the unicast INPUT/JOIN channel while state stayed plaintext — a false sense of security. Real confidentiality would mean an all-unicast redesign (host unicasts STATE to each client), a v2 effort worth doing only if the traffic ever needs to be private.
- **Reliability** — STATE is fire-and-forget and self-healing, and is now also *ordered*: a snapshot
  older than the last one applied is dropped rather than rewinding the client a frame. JOIN and START
  are still send-once, but the failure they used to cause is closed at the other end — a client that
  misses START notices from the host's own beacon (`started`) that a match began without it and drops
  back to browse, instead of spinning forever behind a host-alive timer that STATE frames kept
  refreshing. Retry-until-acked would still be the better fix if lobby joins prove flaky in the room.
- **Player liveness** — a mid-match input blob now expires after 500 ms, so the host stops replaying
  a departed player's last input into the sim every tick. This does **not** stop that player's piece:
  `applyInput` writes into game state, so not calling it just leaves the last value standing — a
  disconnected Tron bike drives straight on. **Swarm answers this inside the game** rather than in
  the stack, in two stages: its sim parks the ship after ~1.2 s of silence, and *removes it from the
  board* after ~3 s. The second stage is not tidiness. A parked ship is still a target, and Swarm's
  lives are shared, so aliens crashing into an abandoned ship were billing the players still there —
  a unit that walked away drained the run for everybody. Removal is free (no life is charged) and
  reverses on the next input. No hook and no heartbeat needed, and the pattern is available to any
  game that wants it. The general version — a `NetGame` hook letting the game choose idle,
  eliminated or AI-controlled, plus a client heartbeat, since a client in the lobby transmits
  nothing at all — is still open.
- **Game-type isolation** — `NetGame::gameId` is now enforced, not just carried: session frames whose
  `gameId` does not match are dropped, and browse lists only lobbies for the runner's own game.
  Discovery is exempt on purpose, since a browser has to hear every host to filter the list.

---

## 9. Phased plan

- **Phase 0 — Transport bring-up:** ✅ folded into `NetLink` (channel-pinned STA, RX queue, broadcast/unicast).
- **Phase 1 — Net service + discovery + lobby page:** ✅ `Net` + `Multiplayer` browse/host/join/roster/start.
- **Phase 2 — NetGame plumbing:** ✅ `NetGame` interface + host-authoritative loop in `Multiplayer::servicePlaying`.
- **Phase 3 — Tron slice:** ✅ `src/games/tron/tron.cpp` (always-forward, absolute-heading stick, A boost, growing trail, last alive wins).
- **Phase 4 — Polish:** ✅ host-leave (timeout→lobby); ✅ **scores/rounds** (host tracks wins, syncs a `MSG_SCORES` scoreboard, result screen shows a per-player win-bar chart); ✅ **user color identity** (replaced the A+B tag — `Storage`-backed preset index, MAC-derived default, chosen in Settings, synced on JOIN, host-resolved with next-free-preset dedup and shipped in `MSG_START` so all screens match). See `ui-restructure-plan.md` for the menu/shell restructure. Encryption de-scoped (see §8). Still deferred **only if needed:** client prediction and true host migration.

## On-hardware test plan (next)

### Tron

1. Flash 2 units. Open **Multiplayer** on both. One shows **H** (host) — press A. The other should show a green player count — press A to join; host count ticks to **2**.
2. Host press A to start → both count **3-2-1** → two trails appear on both screens, mirrored.
3. Verify: the stick turns to the direction pointed, pulling straight back does **not** reverse into your own trail, A boosts, hitting a trail/edge kills, last-alive shows the winner's border on every unit.
4. Add units 3–5 and repeat. Then pull power on the host mid-match — clients should drop back to browse within ~2 s.

*Known first-boot risks to watch:* FastLED (RMT) vs. WiFi timing coexistence, and confirming all units actually settle on channel 1. Both are radio-bring-up issues, not logic issues. On rev 2 add a third: 256 WS2812B idle at ~205 mA before anything is lit, and the rail is under 1 A — watch for brownouts during ESP-NOW TX bursts, and check the boot splash first (see `joystick-16x16-plan.md` §2).

### Swarm

Swarm's rules are covered by `test/test_swarm` and need no board. **What cannot be tested off
hardware is whether the screen is readable**, and that is the whole risk — so look at that first,
before tuning anything.

1. **Solo, one unit.** The submenu: stick left/right picks 1P/MP, up/down cycles the weapon, and the
   glyph shows the *shape* the weapon paints. Enter 1P. You plus one AI wingman.
2. **Read the screen.** Sit through wave 4 (the first with armour) and answer three questions:
   can you find your own ship among four moving pixels; can you tell your bullets from incoming
   fire; can you tell the alien types apart. All three lean on **brightness**, not hue — bright is
   yours, dim wants to kill you. If any answer is no, that is a `waves.h` edit (fewer, larger
   waves) or a palette edit, not a rules problem.
3. **The HUD row** (bottom): three life pixels on the left, wave pips on the right, replaced by a
   boss health bar once the boss arrives.
4. **Each weapon once.** The Laser should visibly charge and flash a whole column; the Bomb should
   detonate on the *near* face of a block and take two rows; the Spread should die out about
   seven cells up; the Shield should hold a 3-wide barrier and drain in about two seconds.
5. **Four units.** Host from the Swarm submenu → MP. Confirm every panel shows the *same* board,
   that no shot ever hurts a teammate, and that one player's death costs the whole squad a life.
6. **Pull power on one client mid-run.** Its ship should vanish within ~3 s and the squad should
   stop losing lives to it. Power it back on: it rejoins at its spawn column. This is the failure
   the shared life pool made expensive, and it is worth provoking deliberately.
7. **Clear the boss.** Every unit should hear the *winning* sting, not just the MVP, and every
   player's bar should go up. That is `teamGame()` working; if three units play the losing sting,
   it is not wired.

*Power:* the busiest frame is ~52 lit pixels, mostly dimmed and single-channel, so Swarm is well
inside the budget — but the Laser's full-column flash and the boss-death blast are the two moments
worth watching a rail during.
