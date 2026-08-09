#pragma once
#include <Arduino.h>
#include "core/game.h"        // Icon
#include "core/display.h"

// ============================================================================
//  Layer 4 -- Game integration. A NetGame is a host-authoritative multiplayer
//  game. The Multiplayer controller drives it; the game never sees the network:
//  it only fills/reads byte buffers and renders.
//
//  The SAME object services both roles. On the host, hostTick() advances the
//  authoritative sim and serializeState() produces the snapshot. On a client,
//  applyState() adopts the host's snapshot. Both render() the current view.
//
//  A client usually only sends a control reading and renders. A LOCKSTEP game
//  is the exception: its clients do real work, because the thing they own
//  cannot be sent to the host at all. Virus is the case -- every player's rule
//  is compiled into their own device, so the host cannot execute it, and what
//  crosses the wire is the decision that rule reached rather than the rule
//  itself. See lockstep() below and docs/plans/virus-multiplayer-plan.md.
//
//  Arena dimensions arrive at begin() (not hard-coded), so one binary runs on
//  the 8x8 today and the 16x16 next-gen panel.
// ============================================================================

// Raw local controls, sampled by the controller and handed to the game so the
// game decides what they mean (keeps the game free of System / pin knowledge).
//
// Both axes are in STICK space: +x is right and +y is UP. Screen rows grow
// down, so a game turning y into a row has to flip it -- but a NetGame is not
// drawing when it reads this, it is deciding a heading, and "up" is the thing
// the player meant. See controls.h for where that flip does happen.
struct LocalInput {
  int16_t x;        // -100..+100, + = right
  int16_t y;        // -100..+100, + = up
  bool    a;        // button A held
  bool    b;        // button B held
};

class NetGame {
public:
  virtual uint8_t gameId()     const = 0;   // stable id; only matching games talk
  virtual uint8_t maxPlayers() const = 0;
  virtual Icon    menuIcon()   const = 0;
  virtual CRGB    playerColor(uint8_t playerId) const = 0;   // for lobby/scoreboard

  // How big a board this game wants. Defaults to the whole panel, which is what
  // every game wanted until one of them needed room for a HUD -- Virus keeps two
  // rows for its territory bar and match clock.
  //
  // The RUNNERS have to ask rather than assume. A runner that hard-codes the
  // panel size hands the game an arena it did not ask for, and the game draws
  // over its own HUD; worse, the wrong number is what goes out in the lobby
  // beacon, so every client agrees on the wrong board.
  virtual uint8_t arenaW() const { return MATRIX_W; }
  virtual uint8_t arenaH() const { return MATRIX_H; }

  // Single-player support. A game that can be played vs AI overrides these; the
  // 1P runner drives AI players through the same applyInput() path as humans.
  virtual bool    supportsSinglePlayer() const { return false; }
  virtual size_t  aiInput(uint8_t playerId, uint8_t* buf, size_t cap){   // default: straight
    (void)playerId;
    if (cap < 2) return 0;
    buf[0] = 0; buf[1] = 0;
    return 2;
  }

  // ---- lockstep -------------------------------------------------------------
  // False by default: the runner samples input on its own clock and ticks on
  // its own clock, the two independent, which is what any game driven by a
  // joystick wants.
  //
  // True means each tick is gated on decisions computed against ONE specific
  // published board. The runner has to tag what it publishes, match the tag on
  // what comes back, and tick anyway when the deadline passes -- treating
  // whatever is missing as "this player did nothing". That last part is not a
  // fallback bolted on for reliability; it is the same path a dropped frame, a
  // slow rule and a unit that walked away all take.
  virtual bool lockstep() const { return false; }

  // Matches per session. 0 = rematch until somebody backs out, which is Tron.
  // Any other N ends the session on a final standings screen after N matches.
  virtual uint8_t seriesRounds() const { return 0; }

  // Longest series this game will accept, or 0 if the length is not the
  // player's to choose. The lobby shows its picker only when this is non-zero,
  // which keeps the shell from having to know which games have series.
  virtual uint8_t maxSeriesRounds() const { return 0; }

  // Ask for a series length, for a game that lets one be chosen. Ignored by
  // default, and a game is free to clamp what it is given. Set before begin().
  //
  // In a networked match only the host has the screen that picks it, so the
  // number travels in MSG_START and every client applies it here -- both roles
  // decide when the series is over on their own, and they have to agree.
  virtual void setSeriesRounds(uint8_t rounds) { (void)rounds; }

  // Which fixed starting layout the NEXT begin() should lay out, for a game
  // that has them. Called with the round number before every match, so a series
  // walks its layouts in order. Default ignores it, which is every game whose
  // start is the same each time or is decided by the seed alone.
  //
  // It rides here rather than in begin()'s arguments because both ends of a
  // networked match have to pick the same one, and the round number is already
  // something they agree on without another word crossing the wire.
  virtual void setOpening(uint8_t round) { (void)round; }

  // ---- co-op ----------------------------------------------------------------
  // A game the players win or LOSE TOGETHER. Default false, because every game
  // up to Swarm had exactly one winner and the runners were built around that:
  // they compare the winner's id against your own to decide whether you hear
  // the winning sting, and they credit the win to that one player.
  //
  // Neither question has an answer in a co-op game. Told true, a runner plays
  // the same sting on every unit and records the result for everyone, so the
  // standings read as runs cleared together. isOver() still names somebody --
  // the result screen wants a border colour and a framed bar -- but nothing
  // punitive hangs off it any more. See docs/plans/swarm-plan.md section 5.
  virtual bool teamGame() const { return false; }

  // ---- lobby identity -------------------------------------------------------
  // One byte this device wants the others to see in the lobby, carried by the
  // backend and never interpreted by it -- the same opacity INPUT and STATE
  // have. Swarm puts the chosen weapon here so a squad can see what it is
  // about to take in and re-pick before anyone presses start. Default 0.
  virtual uint8_t lobbyTag() const { return 0; }

  // Draw another player's tag inside a box the lobby has set aside. Default
  // draws nothing, so a game with no tag simply shows colours -- and the box is
  // small (about 5x2), because five of these have to fit down one 16x16 panel
  // beside a colour swatch each.
  virtual void drawLobbyTag(Display& d, uint8_t tag, const CRGB& c,
                            int x, int y, int w, int h) const {
    (void)d; (void)tag; (void)c; (void)x; (void)y; (void)w; (void)h;
  }

  // Sim tick period in ms. 0 = the runner's shared match cadence, which is what
  // every real-time game wants and what keeps single- and multiplayer feeling
  // identical. A game with its own pace says so -- Virus ticks four times
  // slower than Tron, and running it on Tron's clock would not merely look
  // wrong, it would put a lockstep round trip on the wire 25 times a second.
  // Returned as a number rather than read from match_config.h so that net/ does
  // not acquire a dependency on match/.
  virtual uint16_t tickMs() const { return 0; }

  // ---- private per-player state (host -> that one player) --------------------
  // Default sends nothing, so a game whose whole world is public is untouched.
  //
  // It exists because the public snapshot cannot carry everything for everyone:
  // Virus needs each cell's banked energy to answer canAfford*(), which is 4
  // bits per cell per player and does not fit the frame four times over -- but
  // fits easily once, unicast, for the player it belongs to. Anything else a
  // player may see and the others may not (a hand, a fog of war) belongs here.
  virtual size_t serializePrivate(uint8_t playerId, uint8_t* buf, size_t cap){
    (void)playerId; (void)buf; (void)cap; return 0;
  }
  virtual void applyPrivate(const uint8_t* buf, size_t len){ (void)buf; (void)len; }

  // Match start on both roles. seed makes any randomness identical everywhere;
  // colors is the host-resolved palette (one CRGB per playerId) so all screens
  // render each player in the same color.
  virtual void begin(uint8_t arenaW, uint8_t arenaH, uint8_t myId,
                     uint8_t numPlayers, uint32_t seed, const CRGB* colors) = 0;

  // ---- client side ----
  virtual size_t serializeInput(uint8_t* buf, size_t cap, const LocalInput& in) = 0;
  virtual void   applyState(const uint8_t* buf, size_t len) = 0;

  // ---- host side ----
  virtual void   applyInput(uint8_t playerId, const uint8_t* buf, size_t len) = 0;

  // This device's own raw input, immediately before hostTick(). Default is a
  // no-op: a game whose host reads every player -- including its own -- only
  // through applyInput() (any streamed game; Tron) never needs this. It exists
  // for a lockstep game whose host runs its OWN rule directly inside hostTick()
  // rather than through applyInput() -- Virus is the case, and there the host's
  // own button/joystick never goes through serializeInput() at all, since that
  // path only exists to ship a decision to somebody ELSE's host.
  virtual void   setLocalInput(const LocalInput&) {}

  virtual void   hostTick() = 0;                          // one authoritative step
  virtual size_t serializeState(uint8_t* buf, size_t cap) = 0;

  // ---- both ----
  virtual bool   isOver(uint8_t& winnerId) const = 0;     // host: from sim; client: from state
  virtual void   render(Display& disp) const = 0;

  virtual ~NetGame() {}
};
