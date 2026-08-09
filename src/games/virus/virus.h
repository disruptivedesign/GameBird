#pragma once
#include "net/net_game.h"
#include "net/net_proto.h"     // NET_MAX_PLAYERS, NET_PID_NONE
#include "virus_api.h"
#include "virus_rules.h"

// ============================================================================
//  Virus: a host-authoritative, fully deterministic NetGame. Each player is a
//  pure rule function (see virus_api.h) that the referee runs once per owned
//  cell per tick. Every tick reads a snapshot taken before it starts and
//  applies all funded actions from all players simultaneously (standard
//  cellular-automaton double buffering) -- so no cell can react to another
//  cell's move within the same tick. Own the most cells when the match ends.
//
//  Resolution-agnostic: the board is sized from begin(), so the same code runs
//  on the 8x8 today and a 16x16 panel later with only the tunables retuned.
// ============================================================================

#define VIRUS_MAX_CELLS    256   // 16x16 upper bound
#define VIRUS_TICK_MS      180   // sim tick period (~5.5 Hz); pacing dial
#define VIRUS_STALL_WINDOW 24    // end if nothing NET changes over this many ticks
#define VIRUS_HUD_ROWS     2     // rows below the board: territory bar + clock
// Matches in a session, single-device and networked alike. Lives here rather
// than in virus_app.h because the NetGame contract has to answer for it too.
#define VIRUS_SERIES_ROUNDS 5
// State snapshot header, ahead of the packed cells: phase, winner, numPlayers,
// arenaW, arenaH, then the tick as two bytes.
#define VIRUS_STATE_HDR    7

// How far a virus's centre of mass must drift across the stall window to count
// as "still playing", in sixteenths of a tile. Cells that Move change nobody's
// cell count, so a purely kinetic match would read as stalled on territory
// alone; the centroid is what tells a drifting swarm apart from a frozen one.
#define VIRUS_STALL_DRIFT_Q4 16  // 16/16 == one full tile

// Match length and the stalemate tolerance are now DERIVED from the board size
// (see matchTicksFor / stallEpsFor in the .cpp) rather than fixed, because both
// were tuned against 64 cells and neither survives the move to 224 unchanged.
#define VIRUS_FX_MS        60    // combat flash duration (a third of a tick), fades out over it
#define VIRUS_SPORE_REACH  3     // how many tiles a Spore jumps (sensing + landing both use this)

class Virus : public NetGame {
public:
  uint8_t gameId()     const override { return 2; }
  uint8_t maxPlayers() const override { return 4; }
  Icon    menuIcon()   const override;

  // Two rows shorter than the panel: the bottom pair is the live territory bar
  // and the match clock (see VirusApp::drawHud). Declared HERE rather than at
  // the call site so every runner -- local, networked, and whatever comes next
  // -- gets the same board without having to know why it is short.
  uint8_t arenaH() const override { return MATRIX_H - VIRUS_HUD_ROWS; }
  CRGB    playerColor(uint8_t playerId) const override {
    return playerId < NET_MAX_PLAYERS ? _colors[playerId] : CRGB(255, 255, 255);
  }

  // Assign a rule to each player slot. Call before begin(). rules[i] drives
  // player i (owner code i+1 on the board); virusIdx[i] is that rule's slot in
  // VIRUS_RULES (0..3), used only to label telemetry with the virus's letter.
  void setRoster(const RuleFn* rules, const uint8_t* virusIdx, uint8_t n);

  // Networked: this device brings exactly ONE rule and everyone else's
  // decisions arrive over the wire. Which slot it belongs in is not known when
  // the player picks it -- the lobby has not assigned an id yet -- so the rule
  // is placed by begin(), once myId exists. Every other slot stays null, and a
  // null slot is what tells hostTick to take that player's actions off the wire
  // instead of calling a rule. Cleared by setRoster(), so going back to a
  // single-device match needs no undoing.
  void setLocalRule(RuleFn fn, uint8_t virusIdx);

  void begin(uint8_t arenaW, uint8_t arenaH, uint8_t myId,
             uint8_t numPlayers, uint32_t seed, const CRGB* colors) override;

  // ---- lockstep multiplayer (docs/plans/virus-multiplayer-plan.md) ----------
  // Every player's rule is compiled into their own device, so the host has no
  // code to run for anyone but itself. What crosses the wire is the decision
  // each rule reached: one entry per owned cell, in scan order, no cell indices.
  bool     lockstep()     const override { return true; }
  uint8_t  seriesRounds() const override { return VIRUS_SERIES_ROUNDS; }
  uint16_t tickMs()       const override { return VIRUS_TICK_MS; }

  // Client side. A "player" is still a rule, not a joystick -- LocalInput's x/y
  // go unused -- but button A rides along as World.button for whichever rule
  // this device is running. This emits this device's decision list for the
  // board it last adopted; the runner tags the frame with the tick that board
  // came from, see the header note on NetHeader.tick for why that tag is not
  // optional.
  size_t serializeInput(uint8_t* buf, size_t cap, const LocalInput& in) override {
    return decideLocal(buf, cap, in.a);
  }
  size_t decideLocal(uint8_t* buf, size_t cap, bool buttonA);

  // Host's own button, for the local rule(s) hostTick() calls directly (see
  // NetGame::setLocalInput). A single-device match (VirusApp) calls this
  // straight from System rather than going through a LocalInput at all, since
  // there is no per-player wire framing to bother with when every rule is
  // local.
  void setButtonA(bool held) { _buttonA = held; }
  void setLocalInput(const LocalInput& in) override { setButtonA(in.a); }

  // Host side: stash a remote player's decisions for the next tick. The RUNNER
  // is what checks the tick tag -- by the time a list arrives here it has
  // already been matched to the board about to be simulated, or dropped. A
  // player with no stored list simply idles, which is also what a disconnected
  // one does.
  void   applyInput(uint8_t playerId, const uint8_t* buf, size_t len) override;

  // Host -> one client: what that player's cells may spend on the coming tick,
  // four bits each, in the same owned-cell scan order a decision list uses. The
  // client keeps no energy ledger of its own -- it is told, every tick, so
  // there is nothing on that side that can drift from the referee.
  size_t serializePrivate(uint8_t playerId, uint8_t* buf, size_t cap) override;
  void   applyPrivate(const uint8_t* buf, size_t len) override;

  // Bytes each list occupies for a player owning `cells` cells.
  static size_t decisionBytes(int cells);
  static size_t energyBytes(int cells);

  void   hostTick() override;                          // one authoritative step
  size_t serializeState(uint8_t* buf, size_t cap) override;
  void   applyState(const uint8_t* buf, size_t len) override;

  bool   isOver(uint8_t& winnerId) const override { winnerId = _winner; return _phase != 0; }
  void   render(Display& disp) const override;

  // What happened to one player on the tick just committed, coalesced to counts.
  // Render-only, exactly like _fx: the audio layer reads this to decide what to
  // play, and nothing in the simulation ever does. See events() below.
  struct TickEvents { uint8_t grew, attacked, damaged, lost, moved, spored; };
  const TickEvents& events(uint8_t playerId) const {
    return _ev[playerId < NET_MAX_PLAYERS ? playerId : 0];
  }

  // Live standings, for the territory bar. Counted every tick by evaluateEnd()
  // whether anyone asks or not, so this is a read of existing work.
  uint16_t cellCount(uint8_t playerId) const {
    return _cells[playerId < NET_MAX_PLAYERS ? playerId : 0];
  }
  uint16_t tick()       const { return _tick; }
  uint16_t matchTicks() const { return _matchTicks; }

  // Energy a virus earns per tick, split across the cells it owns. Derived from
  // the board at begin(). Exposed so the tests can assert the economy they were
  // calibrated against, and fail with a clear message if the dial is swept.
  uint16_t income() const { return _income; }

private:
  static constexpr uint16_t CLAIM_NONE = 0xFFFF;   // _claimSrc: claim was a Grow/Spore, not a Move

  uint8_t  _arenaW = 0, _arenaH = 0;
  uint8_t  _myId = 0, _numPlayers = 0;
  uint32_t _seed = 0;
  uint16_t _tick = 0;
  uint8_t  _phase = 0;                    // 0 = running, 1 = over
  uint8_t  _winner = NET_PID_NONE;

  // This device's button A, live -- see ButtonState in virus_api.h. Set by the
  // runner (VirusApp::setButtonA or Multiplayer via setLocalInput) right
  // before the tick that will read it; never serialized, since a client
  // reports it itself and the host has no business relaying one player's
  // button to the others.
  bool     _buttonA = false;

  // Stalemate detection: end the match once nothing a player does moves the
  // needle over the last VIRUS_STALL_WINDOW ticks. Comparing NET counts across
  // a window -- not the owner map tick to tick -- catches a flickering frontier
  // (cells trading back and forth) as well as a frozen one: however violently
  // the seam churns, if no ground is *net* changing hands the game is decided.
  //
  // Territory alone is not enough once cells can Move, because a swarm gliding
  // across the board changes nobody's count. So each player's CENTRE OF MASS is
  // tracked alongside, and a match is stalled only if the counts AND both
  // centroid axes are flat. That distinguishes a swarm going somewhere (worth
  // watching, and still able to reach new ground) from a pair of cells
  // oscillating in place, whose centroid never moves either.
  uint16_t _cntHist[VIRUS_STALL_WINDOW][NET_MAX_PLAYERS] = {{0}};
  uint16_t _sxHist [VIRUS_STALL_WINDOW][NET_MAX_PLAYERS] = {{0}};   // sum of x over owned cells
  uint16_t _syHist [VIRUS_STALL_WINDOW][NET_MAX_PLAYERS] = {{0}};   // sum of y over owned cells
  uint16_t _lastNetDelta = 0xFFFF;   // max per-player |now - window ago|; 0xFFFF until the window fills
  uint16_t _lastDrift    = 0xFFFF;   // max per-player centroid shift, in 1/16 tile

  // Derived from the board at begin(); see the *For helpers in the .cpp.
  uint16_t _income     = 3;
  uint16_t _matchTicks = 160;
  uint16_t _stallEps   = 1;

  // Per-player cell counts as of the last tick. Render-only, like _fx and _ev:
  // the simulation never reads them and they are never serialized.
  uint16_t _cells[NET_MAX_PLAYERS] = { 0 };

  RuleFn   _rule[NET_MAX_PLAYERS] = { nullptr };
  char     _tag[NET_MAX_PLAYERS]  = { '?' };   // virus letter per slot (telemetry)
  RuleFn   _localRule = nullptr;               // networked: our one rule, slot TBD
  uint8_t  _localIdx  = 0;                     // which letter it is, for telemetry
  CRGB     _colors[NET_MAX_PLAYERS];

  // Live board: owner code (0 empty, 1..4 player, 6 neutral) + strength 0..4.
  // Both fit in 3 bits, which is what the packed state snapshot allots them.
  uint8_t  _owner[VIRUS_MAX_CELLS];
  uint8_t  _strength[VIRUS_MAX_CELLS];

  // Each cell's own energy purse, in 1/256ths of an energy unit. A virus's
  // income is split evenly across the cells it owns and credited here, so a
  // cell on a crowded board earns a fraction per tick and banks it until it can
  // afford something. Fixed point rather than a float keeps the sim exactly
  // reproducible. Host-only: clients render the board, they never simulate, so
  // this never crosses the wire.
  uint16_t _cellBank[VIRUS_MAX_CELLS];

  // CLIENT side only: what each of my cells may spend this tick, in whole
  // units, as the host reported it (MSG_PRIVSTATE). Kept separate from
  // _cellBank rather than overloading it, because the two are different things
  // -- _cellBank is the host's authoritative Q8 ledger, this is one tick's
  // spending power handed to a device that does not keep a ledger at all.
  uint8_t  _netEnergy[VIRUS_MAX_CELLS] = { 0 };

  // Decisions received from remote players, still packed. Consumed and cleared
  // by the tick that uses them, so a list can never be replayed onto a board it
  // was not computed against.
  struct Pending { uint8_t buf[NET_INPUT_MAX]; uint8_t len; bool valid; };
  Pending  _pending[NET_MAX_PLAYERS] = {};

  uint16_t _owned[VIRUS_MAX_CELLS];   // scratch: this player's cells, scan order

  // ---- render-only combat feedback (NOT sim state) -------------------------
  // What happened to each cell on the tick just committed, so render() can
  // flash it. Rebuilt every tick and never read by the simulation, never
  // serialized -- determinism does not depend on any of this.
  static constexpr uint8_t FX_DAMAGED = 0x01;   // lost strength but survived
  static constexpr uint8_t FX_DIED    = 0x02;   // was killed (tile is Neutral now)
  uint8_t  _fx[VIRUS_MAX_CELLS] = { 0 };
  uint32_t _tickWallMs = 0;                     // millis() at the last tick; flash phase

  // Per-player event counts for the tick just committed -- the audio layer's
  // equivalent of _fx, and render-only for the same reasons. COUNTS, not
  // events: a virus that grows twelve cells in one tick must not produce twelve
  // sounds, so the collapsing happens here, at the point where the referee
  // still knows the twelve are all the same kind. Counters saturate at 255.
  TickEvents _ev[NET_MAX_PLAYERS] = {};
  static void bump(uint8_t& v) { if (v < 255) v++; }

  // Per-tick scratch (members, not stack: keeps hostTick's frame small).
  uint8_t  _sOwner[VIRUS_MAX_CELLS];      // snapshot owner
  uint8_t  _sStr[VIRUS_MAX_CELLS];        // snapshot strength
  uint16_t _damage[VIRUS_MAX_CELLS];      // attack damage landing on each cell
  uint8_t  _fort[VIRUS_MAX_CELLS];        // 1 if the cell fortified this tick
  uint8_t  _claimBy[VIRUS_MAX_CELLS];     // claimant (0 none, 1..4, 0xFE contested)
  uint16_t _claimSrc[VIRUS_MAX_CELLS];    // for a Move claim, the cell walking in; else CLAIM_NONE
  uint8_t  _died[VIRUS_MAX_CELLS];        // 1 if this cell took lethal damage this tick
  uint8_t  _newStr[VIRUS_MAX_CELLS];      // its strength after damage and fortify
  uint8_t  _vacated[VIRUS_MAX_CELLS];     // 1 if its occupant successfully moved away

  void drawHud(Display& disp) const;    // the two rows the arena gave up
  Cell buildCell(int c, uint8_t player, uint16_t energy) const;  // snapshot view for a rule
  void applyIfLegal(uint8_t player, int c, const Action& a);
  void commitTick();
  void evaluateEnd();

  // THE shared convention: which cells a player owns, and in what order. Both
  // ends of a decision list walk this, and nothing else keeps them in step --
  // so there is exactly one implementation and both roles call it.
  int  collectOwned(const uint8_t* owner, uint8_t code, uint16_t* out) const;
  Phase phaseNow(const uint8_t* owner) const;   // identical on host and client
};
