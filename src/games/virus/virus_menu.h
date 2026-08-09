#pragma once
#include "core/game.h"
#include "core/system.h"
#include "virus.h"
#include "virus_app.h"
#include "match/multiplayer.h"

// ============================================================================
//  VirusMenu: the top-level menu entry for Virus. Two ways to play, the same
//  shape as TronApp -- Single is today's local series where you pick which of
//  your four viruses fight each other, Multiplayer takes ONE of them onto the
//  network to face other people's.
//
//  The extra step compared to TronApp is the picker. Every device compiles its
//  own virus_rules.cpp, so your A and somebody else's A are different code:
//  what you are choosing here is which of YOUR four represents you. Everyone
//  else on the panel is told apart by colour, not by letter, since the letters
//  would collide and mean nothing.
// ============================================================================
class VirusMenu : public Game {
public:
  VirusMenu(System& sys, Virus& game, VirusApp& single, Multiplayer& mp)
    : _sys(sys), _game(game), _single(single), _mp(mp) {}

  void       begin() override;
  GameStatus service() override;
  void       end() override;     // forwards to whichever child is running
  Icon       menuIcon() const override { return _game.menuIcon(); }

private:
  System&      _sys;
  Virus&       _game;
  VirusApp&    _single;
  Multiplayer& _mp;
  Display&     disp() { return _sys.display; }

  enum St { SUB, SINGLE, PICK, MULTI };
  St   _state = SUB;
  int  _sel  = 0;                // submenu page: 0 = single, 1 = multiplayer
  int  _pick = 0;                // 0..3 -> virus A..D
  bool _wantExit = false;

  void serviceSub();
  void servicePick();
};
