#pragma once
#include "net/net_game.h"
#include "net/net_proto.h"   // NET_MAX_PLAYERS, NET_PID_NONE

// ============================================================================
//  Tron: a host-authoritative NetGame. Every player is an always-moving point
//  that lays a finite, slowly growing trail. The joystick picks a HEADING
//  directly, A boosts. Hitting any trail (own included) is instant death; the
//  arena edge gives one step's grace first (see hostTick). Last alive wins.
//  Resolution-agnostic: the grid is sized from begin(),
//  and so are the speed and trail-growth tunables (see the *For helpers in the
//  .cpp) -- a 16x16 arena is four times the ground and would otherwise play
//  like wading.
//
//  ---- Heading, not steering ------------------------------------------------
//  Rev 1 sent a RELATIVE turn (-1/0/+1 applied on each step while held), which
//  was the only thing a single-axis slider could express. A four-way stick can
//  name the direction outright, so the wire now carries the desired heading and
//  the player goes where they pointed. Same two bytes, so the protocol did not
//  resize -- but the meaning of byte 0 changed, which is exactly the kind of
//  silent mismatch NET_PROTO_VER exists to catch. It was bumped for this.
// ============================================================================

#define TRON_MAX_CELLS  256    // 16x16 upper bound
#define TRON_MAX_LEN    128    // per-player trail ring-buffer capacity

// Headings, matching the DX/DY tables in the .cpp.
#define TRON_DIR_R      0
#define TRON_DIR_D      1
#define TRON_DIR_L      2
#define TRON_DIR_U      3
#define TRON_NO_TURN    0xFF   // "no request" -- keep going

class Tron : public NetGame {
public:
  uint8_t gameId()     const override { return 1; }
  uint8_t maxPlayers() const override { return NET_MAX_PLAYERS; }
  Icon    menuIcon()   const override;
  CRGB    playerColor(uint8_t playerId) const override;
  bool    supportsSinglePlayer() const override { return true; }
  size_t  aiInput(uint8_t playerId, uint8_t* buf, size_t cap) override;

  void   begin(uint8_t arenaW, uint8_t arenaH, uint8_t myId,
               uint8_t numPlayers, uint32_t seed, const CRGB* colors) override;

  size_t serializeInput(uint8_t* buf, size_t cap, const LocalInput& in) override;
  void   applyState(const uint8_t* buf, size_t len) override;

  void   applyInput(uint8_t playerId, const uint8_t* buf, size_t len) override;
  void   hostTick() override;
  size_t serializeState(uint8_t* buf, size_t cap) override;

  bool   isOver(uint8_t& winnerId) const override { winnerId = _winner; return _phase != 0; }
  void   render(Display& disp) const override;

private:
  struct Player {
    int16_t  hx, hy;          // head cell
    uint8_t  dir;             // current heading, 0=R 1=D 2=L 3=U
    bool     alive;
    bool     boost;
    uint8_t  want;            // requested heading, or TRON_NO_TURN
    uint8_t  stepCtr;         // sub-tick move accumulator
    bool     edgeGrace;       // already held once at the wall -- next attempt kills
    uint16_t trail[TRON_MAX_LEN];
    uint8_t  head;            // ring index of newest cell
    uint8_t  count;           // cells currently in the trail
  };

  uint8_t  _arenaW = 0, _arenaH = 0;

  // Derived from the arena at begin(), so both roles compute them identically
  // from numbers they both already have. Never serialized.
  uint8_t  _startLen  = 3;
  uint16_t _growTicks = 150;
  uint8_t  _stepBase  = 20, _stepBoost = 10;

  uint8_t  _myId = 0, _numPlayers = 0;
  uint32_t _seed = 0;
  uint16_t _tick = 0;
  uint8_t  _phase = 0;                 // 0 = running, 1 = over
  uint8_t  _winner = NET_PID_NONE;

  Player  _p[NET_MAX_PLAYERS];
  uint8_t _owner[TRON_MAX_CELLS];      // 0 = empty, else playerId+1
  CRGB    _colors[NET_MAX_PLAYERS];    // resolved render palette (from begin)

  int      cell(int x, int y) const { return y * _arenaW + x; }
  int      floodArea(int sx, int sy) const;   // reachable empty cells (AI heuristic)
  uint8_t  maxLenNow() const;
  void     spawn(uint8_t i, int x, int y, uint8_t dir);
  void     trailAdd(uint8_t i, int c);
  void     trailTrim(uint8_t i, uint8_t maxLen);
};
