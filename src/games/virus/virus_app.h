#pragma once
#include "core/game.h"
#include "core/system.h"
#include "virus.h"
#include "virus_voice.h"

// ============================================================================
//  VirusApp: the top-level menu entry for Virus. In single-player you pick
//  which of the four viruses (A-D) fight, then watch them play out on the same
//  board -- there is no live input during a match, since a "player" is a
//  compiled rule. Runs the shared Virus sim as a local host and keeps a running
//  win tally. In the picker B backs out to the main menu; mid-series (from the
//  countdown onward) B instead ends the series early and shows the tally as
//  it stands, matching Multiplayer's Virus session.
// ============================================================================
// A series runs this many matches, then ends and shows the final standings.
#define VIRUS_SERIES_ROUNDS 5

class VirusApp : public Game {
public:
  VirusApp(System& sys, Virus& game) : _sys(sys), _game(game) {}
  void       begin() override;
  GameStatus service() override;
  Icon       menuIcon() const override { return _game.menuIcon(); }

private:
  System&  _sys;
  Virus&   _game;
  Display& disp() { return _sys.display; }

  enum St { SETUP, COUNTDOWN, PLAYING, RESULT, SERIES_OVER };
  St       _state   = SETUP;
  bool     _in[4]   = { true, true, false, false };  // which viruses are entered
  int      _cursor  = 0;                              // 0..3 = virus, 4 = GO
  uint8_t  _numPlayers = 2;
  CRGB     _colors[NET_MAX_PLAYERS];
  uint8_t  _wins[NET_MAX_PLAYERS] = { 0 };
  uint8_t  _roundsPlayed = 0;                         // matches finished this series
  uint8_t  _rounds = VIRUS_ROUNDS_DEFAULT;            // matches this series will run
  uint8_t  _winner  = 0xFF;
  bool     _winRecorded = false;
  bool     _wantExit    = false;
  uint32_t _cdStart = 0, _resultStart = 0, _lastTick = 0;

  // Voices for the roster, indexed by player slot (not by virus letter), plus
  // the one signature this match will play. See playTickVoices().
  const VirusVoice* _voice[NET_MAX_PLAYERS] = { nullptr };
  const Sfx*        _signature = nullptr;

  int     selectedCount() const;
  uint8_t seriesChampion() const;                     // most wins; 0xFF if tied
  void    enterSetup();
  void    startSeries();                              // reset tally + rounds, then play
  void    startMatch();
  void    endSeriesEarly();                           // B mid-series: stop, keep the tally
  void    serviceSetup();
  void    servicePlaying(uint32_t now);
  void    serviceResult(uint32_t now);
  void    playTickVoices();
  void    serviceSeriesOver(uint32_t now);
};
