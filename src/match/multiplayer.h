#pragma once
#include "core/game.h"
#include "core/system.h"
#include "net/net_game.h"

// ============================================================================
//  Multiplayer: the game-agnostic lobby + match runner, exposed as a top-level
//  menu Game. It drives discovery/host/join UI, then runs a chosen NetGame in
//  the host-authoritative loop (inputs up, state snapshots down). Adding a
//  networked game is just registering another NetGame here.
// ============================================================================
class Multiplayer : public Game {
public:
  Multiplayer(System& sys, NetGame* game) : _sys(sys), _game(game) {}
  void       begin() override;
  GameStatus service() override;
  void       end() override;
  Icon       menuIcon() const override;

private:
  System&  _sys;
  NetGame* _game;
  Display& disp() { return _sys.display; }
  Net&     net()  { return _sys.net; }

  enum MPState {
    MP_BROWSE,        // pick host or a lobby to join
    MP_HOST_LOBBY,    // hosting, waiting to start
    MP_CLIENT_WAIT,   // joined, awaiting JOINACK
    MP_CLIENT_LOBBY,  // member, awaiting START
    MP_COUNTDOWN,     // 3-2-1
    MP_PLAYING,       // match running
    MP_RESULT,        // winner + scoreboard
    MP_SERIES_OVER,   // fixed-length session finished: final standings
  };
  MPState  _state = MP_BROWSE;

  int      _sel = 0;           // browse: 0 = Host, 1..N = join lobby N-1
  uint16_t _tick = 0;
  uint32_t _waitStart = 0, _cdStart = 0, _resultStart = 0;
  uint32_t _lastTick = 0, _lastInput = 0;
  bool     _winRecorded = false;
  uint8_t  _winner = 0xFF;
  bool     _wantExit = false;  // browse backed out -> GAME_EXIT to the parent
  // Client lobby: have we seen the host advertise itself as OPEN since we got
  // here? Until we have, a host still flagged "started" is just one that has
  // not reopened after the last match, not one that started without us.
  uint8_t  _matchesPlayed = 0;   // this series; only counted when seriesRounds() > 0

  // ---- lockstep client ------------------------------------------------------
  // The private-state frame for a tick and the board it belongs to are two
  // separate sends, and nothing guarantees which lands first. Hold the private
  // one until a board with the matching tick turns up, rather than applying
  // energy that describes a board we have not adopted.
  uint8_t  _privBuf[NET_PRIV_MAX];
  size_t   _privLen   = 0;
  uint16_t _privTick  = 0;
  bool     _privHeld  = false;

  LocalInput readInput();
  bool arenaMatches(const LobbyInfo& l) const;   // can we play on that board?
  void startGame(uint32_t seed);

  void serviceBrowse(uint32_t now);
  void serviceHostLobby(uint32_t now);
  void serviceClientWait(uint32_t now);
  void serviceClientLobby(uint32_t now);
  void serviceCountdown(uint32_t now);
  void servicePlaying(uint32_t now);           // streamed input, host ticks on its own clock
  void servicePlayingLockstep(uint32_t now);   // clients decide; host gates each tick on them
  void publishBoard();                         // host: the board the next tick will read
  void serviceResult(uint32_t now);
  void serviceSeriesOver(uint32_t now);
  void enterResult(uint32_t now, uint8_t winner);   // one match ended, either loop
  void beginMatch();                                // host: mint a seed and go

  // B mid-series: stop where the tally stands rather than play out every
  // match. Only meaningful for a fixed-length series (seriesRounds() > 0),
  // so it is a no-op for Tron -- true means it fired and the caller should
  // return immediately.
  bool tryEndSeriesEarly();

  void spinner(uint32_t now, const CRGB& c);
  void drawScoreboard(uint32_t now);

  // The lobby roster, drawn identically by the host and by every client. It is
  // one function because the two screens showing DIFFERENT things was the bug:
  // the host saw colour swatches and a client saw a spinner, so nobody who had
  // joined could see who else was there or what they had picked.
  void drawRoster(uint32_t now);
};
