#pragma once
#include "core/game.h"
#include "core/system.h"
#include "net/net_game.h"
#include "match/multiplayer.h"
#include "match/single_player.h"

// ============================================================================
//  TronApp: the top-level menu entry for Tron. Presents a submenu (Single
//  Player placeholder / Multiplayer) and delegates to the shared Multiplayer
//  controller for the networked mode. Backing out returns GAME_EXIT so the
//  shell shows the main menu.
// ============================================================================
class TronApp : public Game {
public:
  TronApp(System& sys, NetGame& game, Multiplayer& mp, SinglePlayer& sp)
    : _sys(sys), _game(game), _mp(mp), _sp(sp) {}
  void       begin() override;
  GameStatus service() override;
  void       end() override;      // forwards to whichever child is running
  Icon       menuIcon() const override { return _game.menuIcon(); }

private:
  System&       _sys;
  NetGame&      _game;
  Multiplayer&  _mp;
  SinglePlayer& _sp;
  Display&      disp() { return _sys.display; }

  enum St { SUB, SINGLE, MULTI };
  St   _state = SUB;
  int  _sel = 0;                // 0 = Single Player, 1 = Multiplayer
  bool _wantExit = false;

  void serviceSub();
};
