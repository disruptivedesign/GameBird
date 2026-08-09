# Audio — Implementation Plan (stingers v1)

Status: **implemented** — both firmware envs build clean, `pio test -e native` stays green
(15/15). **On-hardware tuning pending**, and Phase 0 below is deliberately still open: the duty
curve, the stinger lengths and the octave choices are all guesses until the piezo is on a pin.
Targets a passive piezo on **GPIO 8** driven through a FET.
Design source: the buzzer/audio scoping Q&A, plus a survey of the existing Arduino RTTTL
libraries (see §8).

**What shipped differently from this plan** (the plan is left as written; this is the delta):

| Planned | Shipped | Why |
|---|---|---|
| `Audio` in `src/core/`, catalog in `src/audio/` | **all of it in `src/audio/`** | `System` owns `Net` from `src/net/` already; one home per layer is the established pattern |
| One `sfx.h` | **`sound.h` (POD types) + `sfx.h/.cpp` (catalog)** | `virus_rules.cpp` links in the native env, so the types *and* the catalog must be Arduino-free; `audio/sfx.cpp` is now in the native `build_src_filter` |
| `signature` plays at match start | **plays at GO, in place of `SFX_GO`** | Four viruses cannot all sound a calling card through one piezo. First entered virus with a signature wins; the rest are silent |
| Single-player Virus earshot undecided (§4) | **all viruses audible** | The recommended option — every virus in a spectator match was authored locally |
| Client-side FX diff in `applyState()` | **not done** | Was flagged optional; it is a pre-existing combat-flash gap, not an audio regression. Still open |

## Goal

A single system-wide `Audio` service that plays **short stingers** — beeps, blips and 3–6 note
jingles — so the shell and every game can make noise without owning any hardware knowledge.
Two audiences:

1. **The shell and games** draw on a shared, named catalog (`SFX_SELECT`, `SFX_WIN`, …) so the
   device sounds like one product rather than five.
2. **Virus authors** get their own voice: per-event tones, a signature motif and a victory
   jingle, composed in `virus_rules.cpp` beside the rule they already write — **without
   compromising rule purity or the native test build.**

Explicitly *not* in v1: looping background music. That decision removes the entire
duck/resume half of the mixer (§8).

## Decisions

| Topic | Decision |
|---|---|
| Pin / drive | **GPIO 8**, passive piezo through a **FET**. Free on the S3 Super Mini; 4–7 are taken (`defines.h`). |
| Tone generation | **LEDC direct** (`ledcAttach`/`ledcChangeFrequency`/`ledcWrite`), not `tone()`. Same peripheral, but we need duty control for volume and a guaranteed idle-low pin (§1). |
| Scope | **Stingers only.** No looping music, so the arbiter never needs ducking, resume or a mix bus. |
| Voicing | **Monophonic** — one piezo, one voice. Every design problem below follows from this. |
| Arbitration | **Priority preemption, no queue.** A higher-or-equal priority sound interrupts; a lower one is dropped, not deferred (§3). |
| Multiplayer earshot | **Own events only.** Each unit plays only its local player's events, plus match-wide moments (countdown, someone won). Removes the "4 units all beeping" problem outright. |
| Virus author control | **Maximum** — per-event tones, signature motif, victory jingle, and the freedom to write raw note/duration arrays (§4). |
| Volume | **Off / Low / Med / High**, a Settings page persisted to `STORAGE_NS_SYSTEM` — the key that header already reserves. |
| Dependencies | **None added.** Hand-rolled; ~120 lines of player plus data (§8). |
| Determinism | Audio is **render-only**, in the same category as Virus's existing `_fx` combat flash: never simulated, never serialized, never in a test. |

---

## 1. Hardware layer

**Why not `tone()`.** `tone()`/`noTone()` do exist on ESP32 Arduino core 3.x (implemented over
LEDC), but they hard-code 50% duty and `noTone()` leaves the pin detached. Both matter here:

- **Volume is duty cycle.** On a piezo, loudness tracks the average energy in the square wave,
  and 50% duty is the loudest a square wave gets — there is no gain past it. Volume steps are
  duty steps below 50%.
- **The FET must idle off.** A detached or floating gate can leave the FET partly on, holding DC
  across the piezo — a stalled buzz and wasted current. `ledcWrite(pin, 0)` parks the pin at 0%
  duty, i.e. **hard low**, which is exactly the required idle state. Silence must always be
  written, never merely stopped.

```cpp
// once, in begin()
ledcAttach(AUDIO_PIN, 1000, AUDIO_LEDC_BITS);   // 10-bit; freq is set per note anyway
ledcWrite(AUDIO_PIN, 0);                        // FET off from the first instant

// per note
ledcChangeFrequency(AUDIO_PIN, freq, AUDIO_LEDC_BITS);
ledcWrite(AUDIO_PIN, _dutyForVolume);

// rest, end of sound, mute, and shutdown all take the same path
ledcWrite(AUDIO_PIN, 0);
```

**Resolution.** LEDC's usable bit depth falls as frequency rises (`log2(80 MHz / freq)`). At
10 bits the ceiling is ~78 kHz, far above anything a piezo reproduces, so **10-bit is safe across
the whole range** and gives 1024 duty steps — plenty for four volume levels.

**Volume table** (10-bit, tune on hardware — piezo loudness vs. duty is strongly non-linear, so
these are starting points, not a curve):

| Level | Duty | ~% |
|---|---|---|
| Off | 0 | silent, FET off |
| Low | 24 | ~2% |
| Med | 96 | ~9% |
| High | 512 | 50% — maximum for a square wave |

**Write the stingers high.** A small piezo resonates somewhere around 2–4 kHz and is close to
inaudible in the bass. Notes below ~500 Hz will be felt more than heard, so the catalog should
live in the **C5–C7** octaves. This is a composition constraint, not a code one, and it is the
single most common reason homemade buzzer audio sounds weak.

**No peripheral conflict.** LEDC is independent of the RMT block FastLED drives on the S3, and of
the WiFi radio ESP-NOW rides. Audio is hardware-timed once configured, so it costs nothing per
tick and cannot glitch from a busy loop.

## 2. Service shape

`Audio` joins the existing service pattern verbatim — owned by `System`, begun in `System::begin()`,
polled in `System::update(now)` alongside `Button`, `Slider` and `Net`. Nothing blocks: `update()`
advances a cursor and returns.

```
src/core/audio.h/.cpp    Audio service: LEDC backend, arbiter, update(now)
src/core/notes.h         NOTE_C5 … NOTE_B7 frequency constants
src/audio/sfx.h/.cpp     the shared named catalog (SFX_SELECT, SFX_WIN, …)
```

**Data model.** A sound is a flat array of steps; `freq == 0` is a rest.

```cpp
struct Step { uint16_t freq; uint16_t ms; };      // freq 0 = rest

struct Sfx {
  const Step* steps;
  uint8_t     count;
  uint8_t     priority;                           // see §3
};
```

`const` data lands in flash (`.rodata`) on ESP32 automatically — no `PROGMEM` ceremony, and
authors can write a `Step[]` literal inline and take its address. A six-note stinger is 24 bytes.

**Public surface:**

```cpp
void begin();
void update(uint32_t now);              // called from System::update
void play(const Sfx& s);                // arbitrated; may be dropped
void stop();                            // hard silence, pin low
void setVolume(Volume v);               // Off/Low/Med/High, persists
bool busy() const;
```

## 3. Arbitration — the part worth writing carefully

One piezo means one voice, and the interesting question is not *how to beep* but **what to do
when two things want to beep at once.** That is why this is hand-rolled rather than delegated to
a library (§8) — no RTTTL player has a policy for it.

**Four priority tiers:**

| Tier | Value | Examples |
|---|---|---|
| `PRIO_UI` | 0 | menu move, select, back |
| `PRIO_MINOR` | 1 | a cell grew, a hit landed |
| `PRIO_MAJOR` | 2 | you lost a cell, you spored |
| `PRIO_MATCH` | 3 | countdown, go, win, lose |

**Policy:** `play(s)` starts `s` if nothing is playing, **or** if `s.priority >= current.priority`.
Anything lower is **dropped, not queued.**

Dropping rather than queueing is deliberate. A queue makes sounds play after the moment they
describe has passed — the death blip arriving two seconds late, during the victory screen. For
stingers, stale is worse than absent. `>=` rather than `>` so repeated menu clicks retrigger and
the UI stays responsive.

**Event floods.** Virus ticks at 180 ms (`VIRUS_TICK_MS`) and a healthy virus may grow a dozen
cells in one tick. Twelve grow events per tick would be a machine gun. Two defences, both needed:

1. **Coalesce at the source** — the referee reports *counts per event kind per tick*, and the app
   plays **at most one sound per tick**, picking the highest-priority non-zero kind (§4).
2. **The no-preempt rule** — a stinger already playing is not interrupted by an equal-or-lower
   priority successor, so a sound longer than one tick simply rides through the next tick's
   event.

Consequence for the catalog: **in-game stingers should be ≤150 ms**, comfortably inside a tick.
Longer pieces (victory, countdown) are `PRIO_MATCH`, where nothing competes.

## 4. Virus authors — maximum control, zero purity cost

The constraint is stated plainly in [virus_api.h](../../src/games/virus/virus_api.h): `decide()` is
**pure** — no memory between calls, no state, and it sees only its own neighbourhood. On top of
that, `virus.cpp` compiles for the `native` test env against the shims in `test/shims`, with **no
Arduino calls at all**. A rule that called `play()` would break purity, determinism and the test
build simultaneously.

So authors never emit sound. **They declare a voice, and the referee's existing event stream plays
it.** This is precisely the arrangement already used for the combat flash — `_fx` in
[virus.h](../../src/games/virus/virus.h) is documented as "render-only, never read by the
simulation, never serialized, determinism does not depend on any of this." Audio is a second
consumer of that same idea.

**What an author writes**, beside their rule in `virus_rules.cpp`:

```cpp
// Virus B's voice. nullptr = silent for that event.
static const Step B_GROW[]   = { {NOTE_E6, 30} };
static const Step B_DIE[]    = { {NOTE_A5, 40}, {NOTE_E5, 60} };
static const Step B_WIN[]    = { {NOTE_C6,90}, {NOTE_E6,90}, {NOTE_G6,90}, {NOTE_C7,180} };

const VirusVoice VIRUS_B_VOICE = {
  .onGrow    = SFX(B_GROW,   PRIO_MINOR),
  .onAttack  = nullptr,
  .onDamaged = nullptr,
  .onCellLost= SFX(B_DIE,    PRIO_MAJOR),
  .onSpore   = &SFX_SPORE,          // or borrow from the shared catalog
  .signature = nullptr,             // played once at match start
  .victory   = SFX(B_WIN,    PRIO_MATCH),
};
```

`VIRUS_VOICES[]` is indexed exactly like the existing `VIRUS_RULES[]`, so registration follows a
pattern authors have already met. Authors may point at the shared catalog or write raw
`Step[]` literals — that freedom *is* the "maximum control" answer.

**How events reach the voice.** The referee gains a small per-player, per-tick counter block:

```cpp
struct VirusTickEvents { uint8_t grew, attacked, damaged, lost, spored; };
```

zeroed at the top of each tick, incremented where the referee already knows the answer —
`applyIfLegal()` for funded Grow/Attack/Spore, `commitTick()` for damage and death, attributed to
the cell's owner. `VirusApp` reads the block, picks the highest-priority non-zero kind for the
local slot, and calls `Audio::play()` once. The sim gains **no Arduino dependency and no new
state**; counters are as render-only as `_fx`.

**Two open items this exposes** (both flagged, neither blocking):

- **Whose voice in single-player Virus?** v1 Virus is a spectator match where the local device
  hosts and runs *all* the rules, so "own events only" has no obvious referent. Recommendation:
  in single-player, play **all** viruses' voices through the same one-sound-per-tick arbiter — the
  point of authoring a voice is hearing your virus's personality against the others. In
  multiplayer, `_myId` only, as decided. Say if you'd rather it be `_myId` in both.
- **Clients currently have no event stream.** `_fx` is built in `commitTick()`, which only runs on
  the host; `applyState()` does not rebuild it. So in a networked Virus match, clients neither
  flash nor beep. The fix is cheap and render-only — diff the previous owner/strength map against
  the newly applied snapshot inside `applyState()` — but it is a **pre-existing gap in the combat
  flash**, not something audio introduces. Worth doing in the same branch since audio makes it
  audible; noting it here so it is a decision rather than a surprise.

## 5. Shared catalog

`src/audio/sfx.h` — the device's common voice, so the shell is consistent and games get sensible
defaults for free:

| Name | Tier | Where |
|---|---|---|
| `SFX_MOVE`, `SFX_SELECT`, `SFX_BACK` | UI | `menu`, `settings`, every submenu |
| `SFX_COUNTDOWN`, `SFX_GO` | MATCH | `match_shell.cpp` — one place, so all three runners inherit it |
| `SFX_WIN`, `SFX_LOSE` | MATCH | result screens |
| `SFX_DEATH`, `SFX_SPORE`, `SFX_HIT` | MAJOR/MINOR | game defaults |
| `SFX_BOOT` | MATCH | splash |

Hooking `match_shell` is the highest-leverage single edit: `drawMatchCountdown` is already the one
place all three match runners share, so countdown and go-tones land everywhere at once.

## 6. Settings

A second page beside User Color in [settings.h](../../src/ui/settings.h), following that file's
existing `St` state-enum pattern exactly: slider selects Off/Low/Med/High, **each step previews by
playing `SFX_SELECT` at that volume** (essential — an unheard volume slider is unusable), A saves
to `STORAGE_NS_SYSTEM`. `Audio::begin()` loads it, defaulting to Med.

## 7. Test impact

**None on the native suite.** Audio lives in `src/core` and `src/audio`, neither of which the
`native` env compiles — its `build_src_filter` admits only `tron.cpp`, `virus.cpp` and
`virus_rules.cpp`. The referee's event counters are plain integers, so `virus.cpp` stays
shim-clean and `test_virus` is untouched. The 197-byte state-budget assertion is unaffected because
nothing audio-related is ever serialized.

`virus_rules.cpp` *is* in the native build, so `VirusVoice`/`Step`/`Sfx` must be POD types
declared in a header with no Arduino include. Keeping the type definitions in `src/audio/sfx.h`
free of `<Arduino.h>` is a hard requirement, not a preference.

## 8. Library survey — why hand-rolled

`tone()`/`noTone()` ship in ESP32 Arduino core 3.x over LEDC, so nothing is needed merely to make
noise. For melodies, the notable format is **RTTTL** (Nokia ringtones) — a compact one-line
string with a large existing corpus. Four maintained non-blocking players:

| Library | License | Note |
|---|---|---|
| [AnyRtttl](https://github.com/end2endzone/AnyRtttl) | **MIT** | Pluggable tone/millis callbacks, binary RTTTL |
| [PlayRtttl](https://github.com/ArminJo/PlayRtttl) | GPL-3.0 | Converted to ESP32 core 3.x; ships melody sets |
| [melody-player](https://github.com/fabianoriccardi/melody-player) | LGPL-2.1 | ESP32-only, `playAsync()` |
| [NonBlockingRTTTL](https://github.com/end2endzone/NonBlockingRTTTL) | — | Simplest, AVR-oriented |

**Not adopted**, for three reasons: the project carries exactly one dependency (FastLED) and
hand-rolls the rest; GPL-3.0/LGPL-2.1 are awkward in statically linked firmware; and critically,
**none of them solve arbitration (§3)**, which is the actual design content here — they all assume
they own the buzzer. RTTTL's value is its melody corpus, and **stingers-only v1 needs no corpus.**

If RTTTL is ever wanted (v2 background music, or importing ringtones wholesale), **AnyRtttl is the
one to take** — MIT, and its pluggable `tone`/`noTone` callbacks mean it can drive our LEDC
backend and sit *under* the arbiter rather than replacing it. Noting this so the door stays open.

## 9. Phases

- **Phase 0 — Hardware bring-up.** ⬜ **Still open, and it was meant to come first.** The build
  order below inverted it because no piezo was wired yet, so every number in §1 and every stinger
  in §5 is an educated guess. Confirm FET polarity, confirm the pin idles low, sweep for the
  piezo's loud octaves, then retune.
- **Phase 1 — Player + arbiter.** ✅ `src/audio/` — Step/Sfx model, cursor in `update()`, priority
  preemption, `System` wiring.
- **Phase 2 — Catalog + shell.** ✅ `sfx.*`, menu move/select, splash boot tune (it pumps `Audio`
  itself, since it blocks), `match_shell` countdown + go, win/lose on all three result screens.
- **Phase 3 — Settings page.** ✅ Volume page beside User Color, live preview per step, persisted
  to `STORAGE_NS_SYSTEM`.
- **Phase 4 — Virus voices.** ✅ `VirusVoice` + `VIRUS_VOICES[]` in `virus_rules.cpp`,
  `Virus::TickEvents` counters, `VirusApp::playTickVoices()`, three worked example voices.
  Client-side FX diff not done (see the delta table above).
- **Phase 5 — Tune on hardware.** ⬜ Duty curve, stinger lengths, octave choices. Expect this to
  be the phase that actually determines whether it sounds good.

## 10. Risks

- **Piezo is quiet and thin.** The dominant risk, and it is physical, not software. Mitigated by
  writing high (§1) and by Phase 0 finding the resonant peak before content exists.
- **Duty-based volume is coarse.** Four steps is honest; expect Low and Med to be closer together
  than the numbers suggest.
- **Stinger floods in Virus.** Addressed by source coalescing plus no-preempt (§3), but it is the
  thing most likely to need retuning once it is audible.
- **Deferred:** background music, a queue, RTTTL import, per-game volume, and any second voice.
