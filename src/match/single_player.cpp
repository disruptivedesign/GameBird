#include "single_player.h"
#include "core/palette.h"
#include "match_shell.h"
#include "ui/scoreboard.h"
#include "audio/sfx.h"
#include <esp_random.h>

void SinglePlayer::begin(){
  for (int i = 0; i < NET_MAX_PLAYERS; i++) _wins[i] = 0;
  _wantExit = false;
  startMatch();
}

void SinglePlayer::startMatch(){
  _numPlayers = 2;

  // Player 0 = your chosen color; the AI takes a distinct preset.
  uint8_t me = _sys.net.myColorIndex() % NUM_PRESET_COLORS;
  _colors[0] = PRESET_COLORS[me];
  _colors[1] = PRESET_COLORS[(me + 1) % NUM_PRESET_COLORS];

  _game->begin(_game->arenaW(), _game->arenaH(), 0, _numPlayers, esp_random(), _colors);
  _winRecorded = false;
  _winner = 0xFF;
  _cdStart = millis();
  _state = SP_COUNTDOWN;
}

GameStatus SinglePlayer::service(){
  uint32_t now = millis();
  switch (_state){
    case SP_COUNTDOWN: {
      if (_sys.buttonB.wasPressed()){ _wantExit = true; break; }
      uint32_t el = now - _cdStart;
      if (el >= MATCH_COUNTDOWN_MS){ playMatchGo(_sys.audio); _lastTick = now; _state = SP_PLAYING; break; }
      drawMatchCountdown(disp(), _sys.audio, el);
      break;
    }
    case SP_PLAYING: servicePlaying(now); break;
    case SP_RESULT:  serviceResult(now);  break;
  }
  if (_wantExit){ _wantExit = false; return GAME_EXIT; }
  return GAME_CONTINUE;
}

void SinglePlayer::servicePlaying(uint32_t now){
  if (_sys.buttonB.wasPressed()){ _wantExit = true; return; }   // quit to submenu

  if (now - _lastTick >= MATCH_TICK_MS){
    uint8_t buf[NET_INPUT_MAX];
    // human (player 0)
    LocalInput li{ (int16_t)_sys.joy.x(), (int16_t)_sys.joy.y(),
                   _sys.buttonA.isHeld(), _sys.buttonB.isHeld() };
    _game->applyInput(0, buf, _game->serializeInput(buf, sizeof(buf), li));
    // AI (players 1..N-1)
    for (uint8_t pid = 1; pid < _numPlayers; pid++)
      _game->applyInput(pid, buf, _game->aiInput(pid, buf, sizeof(buf)));
    _game->hostTick();
    _lastTick = now;
  }

  uint8_t winner;
  if (_game->isOver(winner)){
    _winner = winner;
    // Same split as the multiplayer runner: in a team game the human wins when
    // the RUN is cleared, not when their own id comes back. Without this an AI
    // wingman that out-damaged you would take the MVP slot and hand you the
    // losing sting on a run you just finished together.
    const bool team    = _game->teamGame();
    const bool cleared = winner != 0xFF;
    if (!_winRecorded){
      if (team){ if (cleared) for (uint8_t i = 0; i < _numPlayers; i++) _wins[i]++; }
      else if (winner < NET_MAX_PLAYERS) _wins[winner]++;
      _winRecorded = true;
    }
    _sys.audio.play((team ? cleared : winner == 0) ? SFX_WIN : SFX_LOSE);   // human is slot 0
    _resultStart = now;
    _state = SP_RESULT;
  }

  _game->render(disp());     // render() clears internally
  disp().show();
}

void SinglePlayer::serviceResult(uint32_t now){
  uint32_t el = now - _resultStart;
  if (matchResultShowsBoard(el)) _game->render(disp());       // final board + winner border
  else drawWinBars(disp(), _wins, _colors, _numPlayers, _winner, now);
  disp().show();

  if (_sys.buttonB.wasPressed()){ _wantExit = true; return; } // exit to submenu
  if (matchResultReleased(el, _sys.buttonA.wasPressed())) startMatch();
}
