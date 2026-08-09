#pragma once
#include "core/game.h"
#include "core/system.h"
#include "match/multiplayer.h"
#include "match/single_player.h"
#include "swarm.h"

// ============================================================================
//  SwarmApp: the top-level menu entry for Swarm. One screen does two jobs,
//  using the two stick axes the other submenus leave idle:
//
//      stick X   1P / MP
//      stick Y   cycle the weapon
//      A         enter
//      B         back to the main menu
//
//  The loadout belongs HERE rather than in the lobby for a practical reason:
//  the lobby is game-agnostic and shared with Tron and Virus, and clients
//  cannot see each other's roster before START anyway -- not even their
//  colours. Picking on your own device costs nothing and needs nothing new on
//  the wire. The choice persists like the colour preset does, so it is set once
//  and remembered; during a run, B cycles it and the swap lands at a wave break.
// ============================================================================

#define SWM_NS       "swarm"   // NVS namespace
#define SWM_KEY_WPN  "wpn"
#define SWM_KEY_BEST "best"    // furthest wave ever reached on this unit

class SwarmApp : public Game {
public:
  SwarmApp(System& sys, Swarm& game, Multiplayer& mp, SinglePlayer& sp)
    : _sys(sys), _game(game), _mp(mp), _sp(sp) {}
  void       begin() override;
  GameStatus service() override;
  void       end() override;      // forwards to whichever child is running
  Icon       menuIcon() const override { return _game.menuIcon(); }

private:
  System&       _sys;
  Swarm&        _game;
  Multiplayer&  _mp;
  SinglePlayer& _sp;
  Display&      disp() { return _sys.display; }

  enum St { SUB, SINGLE, MULTI };
  St   _state = SUB;
  int  _sel = 0;                // 0 = Single Player, 1 = Multiplayer
  bool _wantExit = false;

  // Furthest wave ever reached on this unit, persisted like every other game's
  // high score. Recorded from BOTH modes on purpose: a co-op run is the main
  // event here, so "the furthest we ever got with me in the squad" is the
  // number worth keeping, not a solo-only one.
  uint8_t _best = 0;
  bool    _recorded = false;    // latched per run, cleared when the next begins

  void serviceSub();
  void recordBest();                        // called while a match is running
  void drawWeapon(int x0, int row) const;   // the 5x5 glyph for the pick
};
