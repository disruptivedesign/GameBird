#pragma once
#include "core/game.h"
#include "core/system.h"
#include "net/net_game.h"

// ============================================================================
//  SinglePlayer: a game-agnostic local match runner. It plays the shared game
//  sim as a local host (no networking) with the human as player 0 and AI
//  opponents (player 1..N-1) driven through the game's aiInput(). Reuses the
//  game's begin/applyInput/hostTick/isOver/render verbatim, so gameplay stays
//  identical to multiplayer. Keeps a running win tally; B exits to the caller.
// ============================================================================
class SinglePlayer : public Game {
public:
  SinglePlayer(System& sys, NetGame* game) : _sys(sys), _game(game) {}
  void       begin() override;
  GameStatus service() override;
  Icon       menuIcon() const override { return _game->menuIcon(); }

private:
  System&  _sys;
  NetGame* _game;
  Display& disp() { return _sys.display; }

  enum St { SP_COUNTDOWN, SP_PLAYING, SP_RESULT };
  St       _state = SP_COUNTDOWN;
  uint8_t  _numPlayers = 2;            // 1 human + 1 AI
  CRGB     _colors[NET_MAX_PLAYERS];
  uint8_t  _wins[NET_MAX_PLAYERS] = {0};
  uint8_t  _winner = 0xFF;
  bool     _winRecorded = false;
  bool     _wantExit = false;
  uint32_t _cdStart = 0, _resultStart = 0, _lastTick = 0;

  void startMatch();
  void servicePlaying(uint32_t now);
  void serviceResult(uint32_t now);
};
