#include "virus_app.h"
#include "core/palette.h"
#include "match/match_shell.h"
#include "ui/scoreboard.h"
#include "ui/widgets.h"
#include "audio/sfx.h"
#include <esp_random.h>

// SETUP layout: four virus slots plus a GO slot.
#define VIRUS_SETUP_SLOTS  5
#define LETTER_ROW         4      // the 3x5 letter naming each virus
#define BAR_ROW           11      // in/out bar beneath it
#define ROUNDS_ROW        13      // series-length pips, between the bars and GO

// ---- Live HUD ---------------------------------------------------------------
// Two rows off the bottom of the board: a territory bar and a match clock.
//
// Virus is a spectator game -- you set the roster and then watch -- and until
// now the only way to tell who was winning was to eyeball the board and count.
// The territory bar is the standings, redrawn every tick, for one row. That is
// the best trade on the whole device: no other game gets a live score this
// cheaply, because no other game already counts it.
//
// The board height that leaves room for them is Virus::arenaH(), declared on the
// game itself so every runner gets it -- this app, the networked one, and
// whatever comes next. VIRUS_HUD_ROWS lives in virus.h for the same reason.

void VirusApp::begin() {
  enterSetup();
  _wantExit = false;
}

int VirusApp::selectedCount() const {
  int n = 0;
  for (int i = 0; i < 4; i++) if (_in[i]) n++;
  return n;
}

// Return to the picker and clear the series (a new roster starts fresh).
void VirusApp::enterSetup() {
  for (int i = 0; i < NET_MAX_PLAYERS; i++) _wins[i] = 0;
  _roundsPlayed = 0;
  _state = SETUP;
}

// Most wins this series; 0xFF if two or more are tied at the top.
uint8_t VirusApp::seriesChampion() const {
  uint8_t best = 0xFF; int bestW = -1; bool tie = false;
  for (uint8_t p = 0; p < _numPlayers; p++) {
    if      ((int)_wins[p] > bestW) { bestW = _wins[p]; best = p; tie = false; }
    else if ((int)_wins[p] == bestW) tie = true;
  }
  return tie ? 0xFF : best;
}

// Start a fresh series (same roster): zero the tally + round count, then play.
void VirusApp::startSeries() {
  for (int i = 0; i < NET_MAX_PLAYERS; i++) _wins[i] = 0;
  _roundsPlayed = 0;
  startMatch();
}

// B from the countdown onward: stop the series where it stands rather than
// play out every round, but keep the tally -- the same screen a series that
// reached VIRUS_SERIES_ROUNDS would show. Unlike enterSetup(), this does not
// zero _wins or _roundsPlayed.
void VirusApp::endSeriesEarly() {
  _sys.audio.play(SFX_BACK);
  _resultStart = millis();
  _state = SERIES_OVER;
}

GameStatus VirusApp::service() {
  uint32_t now = millis();
  switch (_state) {
    case SETUP: serviceSetup(); break;
    case COUNTDOWN: {
      if (_sys.buttonB.wasPressed()) { endSeriesEarly(); break; }
      uint32_t el = now - _cdStart;
      if (el >= MATCH_COUNTDOWN_MS) {
        if (_signature) _sys.audio.play(*_signature);   // a virus's calling card, in place of GO
        else            playMatchGo(_sys.audio);
        _lastTick = now; _state = PLAYING; break;
      }
      drawMatchCountdown(disp(), _sys.audio, el);
      break;
    }
    case PLAYING:     servicePlaying(now);    break;
    case RESULT:      serviceResult(now);     break;
    case SERIES_OVER: serviceSeriesOver(now); break;
  }
  if (_wantExit) { _wantExit = false; return GAME_EXIT; }
  return GAME_CONTINUE;
}

// SETUP: left/right moves the cursor across the four viruses and a GO slot; A
// toggles the highlighted virus in/out, or starts the match from GO (needs >=2
// viruses). UP/DOWN sets how many matches the series runs. B exits to the main
// menu.
void VirusApp::serviceSetup() {
  if (int8_t s = _sys.joy.stepX()) {
    _cursor = (_cursor + s + VIRUS_SETUP_SLOTS) % VIRUS_SETUP_SLOTS;
    _sys.audio.play(SFX_MOVE);
  }

  // Series length on the other axis, so it needs no slot of its own and cannot
  // be missed on the way to GO. stepY is screen space -- positive is DOWN a row
  // -- and pushing UP for more rounds is the way round that matches the bar
  // filling upward.
  if (int8_t s = _sys.joy.stepY()) {
    const int want = (int)_rounds - s;
    if (want >= VIRUS_ROUNDS_MIN && want <= VIRUS_SERIES_ROUNDS) {
      _rounds = (uint8_t)want;
      _sys.audio.play(SFX_MOVE);
    }
  }

  if (_sys.buttonB.wasPressed()) { _sys.audio.play(SFX_BACK); _wantExit = true; return; }
  if (_sys.buttonA.wasPressed()) {
    if (_cursor < 4) {
      _in[_cursor] = !_in[_cursor];
      // The catalog's rising/falling pair already reads as in/out everywhere
      // else on the device, so entering and withdrawing a virus can borrow it.
      _sys.audio.play(_in[_cursor] ? SFX_SELECT : SFX_BACK);
    } else if (selectedCount() >= 2) {
      _sys.audio.play(SFX_SELECT);
      startMatch();
      return;
    }
  }

  disp().clear();
  // Four slots across the panel. At 16 wide each is four columns, so the 3x5
  // font fits a slot exactly and each virus can finally be LABELLED with its
  // letter rather than being an anonymous coloured block -- which is how the
  // rules file, the telemetry and the design record have always referred to
  // them. Full colour when entered, dim when out.
  const int slot = MATRIX_W / 4;
  for (int i = 0; i < 4; i++) {
    const int  x = i * slot;
    const CRGB c = _in[i] ? PRESET_COLORS[i] : CRGB(22, 22, 22);

    disp().drawChar((char)('A' + i), x, LETTER_ROW, c);
    for (int row = BAR_ROW; row < BAR_ROW + 2; row++)
      for (int k = 0; k < 3; k++) disp().setPixel(x + k, row, c);

    if (_cursor == i)
      for (int k = 0; k < 3; k++) disp().setPixel(x + k, 0, CRGB(255, 255, 255));
  }

  // Series length, on the one free row between the in/out bars and GO: five
  // pips, lit up to the chosen count. A count reads better as "how many are
  // on" than as "which one is highlighted", so this is not drawSelectorDots.
  drawCountPips(disp(), ROUNDS_ROW, VIRUS_SERIES_ROUNDS, _rounds);

  // GO bar across the bottom: green when startable (bright if focused), else red.
  const bool startable = selectedCount() >= 2;
  const CRGB go = startable ? (_cursor == 4 ? CRGB(0, 255, 0) : CRGB(0, 70, 0))
                            : CRGB(70, 0, 0);
  for (int row = MATRIX_H - 2; row < MATRIX_H; row++)
    for (int col = 0; col < MATRIX_W; col++) disp().setPixel(col, row, go);
  disp().show();
}

// Build the roster from the entered viruses and kick off a match.
void VirusApp::startMatch() {
  RuleFn  rules[NET_MAX_PLAYERS];
  uint8_t idx[NET_MAX_PLAYERS];
  _numPlayers = 0;
  _signature  = nullptr;
  for (int i = 0; i < 4; i++) {
    if (!_in[i]) continue;
    _colors[_numPlayers] = PRESET_COLORS[i];
    rules[_numPlayers]   = VIRUS_RULES[i];
    idx[_numPlayers]     = (uint8_t)i;
    _voice[_numPlayers]  = VIRUS_VOICES[i];
    // One buzzer means one calling card: the first entered virus that declares
    // a signature gets it, and it stands in for the generic go-tone.
    if (!_signature && _voice[_numPlayers]) _signature = _voice[_numPlayers]->signature;
    _numPlayers++;
  }
  for (int i = _numPlayers; i < NET_MAX_PLAYERS; i++) _voice[i] = nullptr;
  _game.setRoster(rules, idx, _numPlayers);
  _game.setSeriesRounds(_rounds);
  // One opening per round, so a full-length series shows each of the five
  // exactly once and every virus has been through every starting spot. A
  // shorter series simply plays the first _rounds of them.
  _game.setOpening(_roundsPlayed);
  _game.begin(_game.arenaW(), _game.arenaH(), 0, _numPlayers, esp_random(), _colors);

  _winRecorded = false;
  _winner      = 0xFF;
  _cdStart     = millis();
  _state       = COUNTDOWN;
}

void VirusApp::servicePlaying(uint32_t now) {
  if (_sys.buttonB.wasPressed()) { endSeriesEarly(); return; }   // end the series, keep the tally

  if (now - _lastTick >= VIRUS_TICK_MS) {
    // Every rule on this device shares the one physical button -- see
    // ButtonState in virus_api.h.
    _game.setButtonA(_sys.buttonA.isHeld());
    _game.hostTick();
    _lastTick = now;
  }

  uint8_t winner;
  if (_game.isOver(winner)) {
    _winner = winner;
    if (!_winRecorded) {
      if (winner < NET_MAX_PLAYERS) _wins[winner]++;
      _roundsPlayed++;
      _winRecorded = true;
    }
    // The winner's own fanfare if its author wrote one, else the house tune.
    const VirusVoice* wv = (winner < _numPlayers) ? _voice[winner] : nullptr;
    _sys.audio.play((wv && wv->victory) ? *wv->victory : SFX_WIN);

    _resultStart = now;
    _state = RESULT;
  }

  _game.render(disp());   // Virus draws its own HUD into the two rows it reserved
  disp().show();
}


void VirusApp::serviceResult(uint32_t now) {
  uint32_t el = now - _resultStart;
  if (matchResultShowsBoard(el)) _game.render(disp());        // final board + border
  else drawWinBars(disp(), _wins, _colors, _numPlayers, _winner, now);
  disp().show();

  if (_sys.buttonB.wasPressed()) { endSeriesEarly(); return; }  // end the series, keep the tally
  if (matchResultReleased(el, _sys.buttonA.wasPressed())) {
    if (_roundsPlayed >= _rounds) { _resultStart = now; _state = SERIES_OVER; }
    else                            startMatch();  // next round, same roster
  }
}

// Series finished: hold on the final standings, the champion's bar blinking and
// framed in its color. A starts a fresh series (same roster); B re-picks.
void VirusApp::serviceSeriesOver(uint32_t now) {
  uint8_t champ = seriesChampion();
  drawWinBars(disp(), _wins, _colors, _numPlayers, champ, now);
  if (champ != 0xFF) disp().border(_colors[champ]);
  disp().show();

  if (_sys.buttonB.wasPressed()) { enterSetup();  return; }    // back to the picker
  if (_sys.buttonA.wasPressed()) { startSeries();  return; }   // play another series
}
