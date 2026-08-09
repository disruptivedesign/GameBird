# Design records

Point-in-time plans for work that has since shipped. They are kept for the
*why* — the decisions, the Q&A behind them, and what was deliberately left
out — not as a description of how the code looks now.

**These are not maintained.** For current structure and behaviour, read
[`docs/architecture.md`](../architecture.md) and the code.

| Record | Covers |
|---|---|
| [single-player-plan.md](single-player-plan.md) | Tron vs AI, reusing the multiplayer `Tron` sim unchanged |
| [ui-restructure-plan.md](ui-restructure-plan.md) | Games-only main menu, per-game submenus, user color identity |
| [virus-plan.md](virus-plan.md) | The Virus ruleset, referee design, and v1 scope |
| [virus-v2-plan.md](virus-v2-plan.md) | Author feedback on v1: the Move action and per-cell energy replacing the shared pool |
| [virus-multiplayer-plan.md](virus-multiplayer-plan.md) | Networked Virus: rules stay on their author's device, so decisions cross the wire instead |
| [audio-plan.md](audio-plan.md) | Piezo buzzer service, stinger catalog, and per-virus voices |
| [breakout-plan.md](breakout-plan.md) | Breakout on 64 pixels: the sub-pixel ball, the six-direction fan, and the 16×16 path |
| [joystick-16x16-plan.md](joystick-16x16-plan.md) | The rev 2 board: joystick replaces the slider, 16×16 WS2812B, and what each game did with the space |
| [swarm-plan.md](swarm-plan.md) | Swarm: the first co-op game, the five weapons you pick between, and what winning *together* cost the match runners |

> **Note on paths:** these records were written against the old flat `src/`
> layout, so file references in them (`src/tron.cpp`, `src/virus_rules.cpp`, …)
> predate the move into `src/core/`, `src/net/`, `src/match/`, and `src/games/`.
> They have been left as written rather than rewritten, since the point of a
> record is what was true when it was made.
