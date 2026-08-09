#include "multiplayer.h"
#include "core/palette.h"
#include "match_shell.h"
#include "ui/scoreboard.h"
#include "ui/widgets.h"
#include "audio/sfx.h"
#include <esp_random.h>

// ---- match cadence (shared with single-player; see match_config.h) ----
#define INPUT_MS       MATCH_INPUT_MS
#define HOST_TICK_MS   MATCH_TICK_MS
#define COUNTDOWN_MS   MATCH_COUNTDOWN_MS
#define MIN_PLAYERS    2
#define BROWSE_SCALE   2      // the 3x5 font is lost at 1x on a 16-tall panel

// ---- menu icon: a wireless / broadcast glyph ----
static const uint8_t IMG_MULTI[8][8] = {
  {0,0,0,1,1,0,0,0},
  {0,1,1,0,0,1,1,0},
  {1,0,0,1,1,0,0,1},
  {0,0,1,0,0,1,0,0},
  {0,0,0,0,0,0,0,0},
  {0,0,0,1,1,0,0,0},
  {0,0,0,1,1,0,0,0},
  {0,0,0,0,0,0,0,0},
};
static const CRGB MULTI_PAL[] = { CRGB::Black, CRGB(0, 200, 255) };

Icon Multiplayer::menuIcon() const { return { IMG_MULTI, MULTI_PAL }; }

// ============================================================================
//  Lifecycle
// ============================================================================
void Multiplayer::begin(){
  net().leave();               // clear any stale session from a previous entry
  // Discovery hears every host on the channel, including ones running a
  // different game. This runner can only play the game it was constructed with,
  // so say so before browsing -- otherwise the list offers lobbies that would
  // be refused, or worse, joined.
  net().browseFor(_game->gameId());
  // Published with our JOIN and then carried in the host's beacon, so it has to
  // be set before either happens. Read from the game rather than stored here:
  // the runner has no idea what the byte means.
  net().setLocalTag(_game->lobbyTag());
  _state = MP_BROWSE;
  _sel = 0;
  _wantExit = false;
  _matchesPlayed = 0;
}

// begin() already clears a stale session, so this is not about the next entry --
// it is about the gap. A host that walks away mid-match without leaving keeps
// beaconing a lobby nobody is in, and its peers hold a roster slot for a device
// that is gone. The forced quit made that gap reachable, so it has to close.
void Multiplayer::end(){
  net().leave();
  _state = MP_BROWSE;
  _wantExit = false;
}

// Can we actually play on this host's board?
//
// The lobby beacon has always carried arenaW/arenaH, and net_proto.h has always
// claimed that made a mixed-resolution match "fail loudly" -- but nothing ever
// compared them. The client simply adopted whatever arrived (see Net's JOINACK
// and START handling), so an 8x8 unit joining a 16x16 host would take a 256-cell
// board, render the top-left quarter of it, and lose to a game it could not see.
// Silently. That is the worst of both worlds: self-describing data that nobody
// reads.
//
// Checked here rather than in Net because Net has no idea what the game wants;
// the arena is the NetGame's to declare (see NetGame::arenaW). Checked at the
// point of JOINING rather than at start, so the refusal lands on the button
// press that caused it.
bool Multiplayer::arenaMatches(const LobbyInfo& l) const {
  return l.arenaW == _game->arenaW() && l.arenaH == _game->arenaH();
}

LocalInput Multiplayer::readInput(){
  return { (int16_t)_sys.joy.x(), (int16_t)_sys.joy.y(),
           _sys.buttonA.isHeld(), _sys.buttonB.isHeld() };
}

// One match ended. Shared by both play loops so the tally cannot end up
// counted in one and not the other.
void Multiplayer::enterResult(uint32_t now, uint8_t winner){
  _winner = winner;

  // A co-op game is won or lost by everyone at once (NetGame::teamGame()), so
  // both questions this function normally asks have to change. "Who won?"
  // becomes "did the run get cleared?", which is what a non-NONE winner means
  // there -- the id itself is only an MVP for the result screen to frame. And
  // "was it me?" stops being asked at all: three of four players hearing the
  // losing sting on a cleared run is the exact wrongness this hook exists for.
  const bool team = _game->teamGame();
  const bool cleared = winner != 0xFF;

  if (net().role() == ROLE_HOST && !_winRecorded && cleared){
    if (team) net().recordTeamWin();
    else      net().recordWin(winner);        // host owns the scoreboard
    _winRecorded = true;
  }
  if (_game->seriesRounds()) _matchesPlayed++;
  _sys.audio.play((team ? cleared : winner == net().myId()) ? SFX_WIN : SFX_LOSE);
  _resultStart = now;
  _state = MP_RESULT;
}

// Jump straight to the series standings, keeping whatever the tally already
// says. The peer is not told: a client that bails leaves the host ticking
// against a player who has gone quiet, same as any dropped connection, and a
// host that bails leaves the client to the existing "host gone" timeout a
// couple of seconds later (see msSinceHost() above) rather than a tally
// screen of its own. A real handshake would need a new wire message for what
// is, so far, one game's escape hatch -- Tron has no series and never reaches
// this at all.
bool Multiplayer::tryEndSeriesEarly(){
  if (!_game->seriesRounds()) return false;
  _sys.audio.play(SFX_BACK);
  _state = MP_SERIES_OVER;
  return true;
}

// Host: mint a seed, tell everyone, and go. Every match in a series goes
// through MSG_START rather than the host quietly rolling on -- clients key a
// good deal off that frame, not least resetting the snapshot ordering, since
// the host's tick counter restarts at 0 and every frame of match two would
// otherwise be rejected as stale.
void Multiplayer::beginMatch(){
  uint32_t seed = esp_random();
  net().startMatch(seed);
  startGame(seed);
}

void Multiplayer::startGame(uint32_t seed){
  // Second gate. Browse checks the lobby BEACON; this checks what actually
  // arrived in MSG_START, which is a different frame and can disagree with it --
  // a host that re-opened on a different board, or a stale session. Playing on
  // an arena we did not agree to means rendering a game we cannot see, so drop
  // back to browse instead.
  if (net().arenaW() != _game->arenaW() || net().arenaH() != _game->arenaH()){
    Serial.printf("[MP] refusing match: host arena %ux%u, we need %ux%u\n",
                  net().arenaW(), net().arenaH(), _game->arenaW(), _game->arenaH());
    _sys.audio.play(SFX_BACK);
    net().leave();
    _state = MP_BROWSE;
    return;
  }

  _game->begin(net().arenaW(), net().arenaH(), net().myId(),
               net().matchPlayers(), seed, net().colors());
  _winRecorded = false;
  _winner = 0xFF;
  _cdStart = millis();
  _state = MP_COUNTDOWN;
}

GameStatus Multiplayer::service(){
  uint32_t now = millis();
  switch (_state){
    case MP_BROWSE:       serviceBrowse(now);      break;
    case MP_HOST_LOBBY:   serviceHostLobby(now);   break;
    case MP_CLIENT_WAIT:  serviceClientWait(now);  break;
    case MP_CLIENT_LOBBY: serviceClientLobby(now); break;
    case MP_COUNTDOWN:    serviceCountdown(now);   break;
    case MP_PLAYING:
      if (_game->lockstep()) servicePlayingLockstep(now);
      else                   servicePlaying(now);
      break;
    case MP_RESULT:       serviceResult(now);      break;
    case MP_SERIES_OVER:  serviceSeriesOver(now);  break;
  }
  if (_wantExit){ _wantExit = false; return GAME_EXIT; }
  return GAME_CONTINUE;
}

// ============================================================================
//  Browse: one stepped list -> [Host] then each open lobby. A acts, B backs out
//  to the parent (the game's submenu).
// ============================================================================
void Multiplayer::serviceBrowse(uint32_t now){
  int lc = net().openLobbyCount();
  int options = lc + 1;                            // 0 = Host, 1..lc = join lobby-1

  // Lobbies appear and disappear as beacons arrive and go stale, so `options`
  // moves underneath the selection. Rev 1 re-derived the index from the slider
  // every frame and was self-correcting by accident; a held index has to be
  // clamped on purpose, before it is used to look anything up.
  if (_sel >= options) _sel = options - 1;
  if (_sel < 0)        _sel = 0;

  if (int8_t s = _sys.joy.stepX()){
    _sel = (_sel + s + options) % options;
    _sys.audio.play(SFX_MOVE);
  }

  if (_sys.buttonB.wasPressed()){
    _sys.audio.play(SFX_BACK);
    net().leave();
    _wantExit = true;
    return;
  }

  if (_sys.buttonA.wasPressed()){
    if (_sel == 0){
      _sys.audio.play(SFX_SELECT);
      net().host(_game->gameId(), _game->maxPlayers(), _game->arenaW(), _game->arenaH());
      _matchesPlayed = 0;              // a fresh lobby is a fresh series
      _state = MP_HOST_LOBBY;
    } else {
      // Only confirm if the lobby is still there -- it may have gone stale
      // between the render and the press.
      const LobbyInfo* L = net().openLobbyAt(_sel - 1);
      if (L && !arenaMatches(*L)){
        _sys.audio.play(SFX_BACK);          // refused; the swatch was already red
      } else if (L){
        _sys.audio.play(SFX_SELECT);
        net().join(*L);
        _waitStart = now;
        _state = MP_CLIENT_WAIT;
      }
    }
    return;
  }

  // render
  disp().clear();
  if (_sel == 0){
    // Host, as a big H in my color.
    const int w = disp().textWidth("H", BROWSE_SCALE);
    disp().drawChar('H', (MATRIX_W - w) / 2, (MATRIX_H - 5 * BROWSE_SCALE) / 2 - 1,
                    PRESET_COLORS[net().myColorIndex()], BROWSE_SCALE);
  } else {
    const LobbyInfo* L = net().openLobbyAt(_sel - 1);
    if (L){                                         // host's color as a swatch block
      // A lobby we cannot join reads as a red block rather than the host's
      // colour, so the refusal is visible before the button is pressed.
      const bool ok = arenaMatches(*L);
      CRGB c = ok ? PRESET_COLORS[L->colorIndex % NUM_PRESET_COLORS] : CRGB(90, 0, 0);
      for (int yy = 4; yy < MATRIX_H - 4; yy++)
        for (int xx = 4; xx < MATRIX_W - 4; xx++) disp().setPixel(xx, yy, c);
      // How full it already is -- one pip per player along the top. Worth the
      // row now there is one to spare: joining a lobby that turns out to be full
      // is the one browse outcome with nothing to show for it.
      for (int k = 0; k < L->playerCount && k < MATRIX_W; k++)
        disp().setPixel(k, 0, CRGB(0, 120, 120));
    }
  }
  drawSelectorDots(disp(), MATRIX_H - 1, options, _sel);
  disp().show();
}

// ============================================================================
//  Host lobby: wait for players, start when >= MIN_PLAYERS.
// ============================================================================
void Multiplayer::serviceHostLobby(uint32_t now){
  uint8_t pc = net().playerCount();

  if (_sys.buttonB.wasPressed()){
    _sys.audio.play(SFX_BACK);
    net().leave();
    _state = MP_BROWSE;
    return;
  }
  // No SFX_SELECT on the start press: startGame() goes straight to the
  // countdown, whose first SFX_COUNTDOWN (PRIO_MATCH) would stomp a PRIO_UI
  // blip on the very next tick anyway. The countdown IS the confirmation.
  bool startReq = _sys.buttonA.wasPressed() && pc >= MIN_PLAYERS;

  if (startReq){
    uint32_t seed = esp_random();
    net().startMatch(seed);
    startGame(seed);
    return;
  }

  drawRoster(now);
  if (pc >= MIN_PLAYERS && ((now / 400) & 1))
    disp().setPixel(MATRIX_W - 1, 0, CRGB(0, 220, 60));   // enough to start
  disp().show();
}

// ============================================================================
//  The roster. One row per occupied slot: a colour block for identity, then
//  whatever the game wants to say about that player, then a white pip on your
//  own row so you can find yourself among four near-identical lines.
//
//  Iterates SLOTS, not 0..playerCount-1. The count is a count: the player in
//  slot 1 leaving while slot 2 stays leaves two players occupying {0,2}, and
//  the old swatch strip drew slot 1's empty seat and missed slot 2 entirely.
// ============================================================================
void Multiplayer::drawRoster(uint32_t now){
  disp().clear();

  const int step  = MATRIX_H / NET_MAX_PLAYERS;                 // 3 at 16 tall
  const int hEnt  = step > 1 ? step - 1 : 1;                    // leave a gap
  const int swW   = MATRIX_W / 5;                               // colour block
  const int tagX  = swW + 1;
  const int tagW  = MATRIX_W - tagX - 2;                        // room for the "you" pip

  int row = 0;
  for (uint8_t pid = 0; pid < NET_MAX_PLAYERS; pid++){
    if (!net().slotUsed(pid)) continue;
    const int y = row * step;
    if (y >= MATRIX_H) break;

    const CRGB c = PRESET_COLORS[net().colorIndexOf(pid) % NUM_PRESET_COLORS];
    for (int dy = 0; dy < hEnt; dy++)
      for (int x = 0; x < swW; x++) disp().setPixel(x, y + dy, c);

    _game->drawLobbyTag(disp(), net().tagOf(pid), c, tagX, y, tagW, hEnt);

    if (pid == net().myId()) disp().setPixel(MATRIX_W - 1, y, CRGB(200, 200, 200));
    row++;
  }
}

// ============================================================================
//  Client: joined -> await JOINACK -> await START.
// ============================================================================
// Every path out of here lands back on browse, and each one gets a sound: a
// silent bounce back to the lobby list is indistinguishable from a button that
// did not register, which is the worst thing a join can feel like.
void Multiplayer::serviceClientWait(uint32_t now){
  if (_sys.buttonB.wasPressed()){ _sys.audio.play(SFX_BACK); net().leave(); _state = MP_BROWSE; return; }
  if (net().takeJoinAck()){
    _sys.audio.play(SFX_SELECT);
    net().clearStateHeard();             // see serviceClientLobby
    // Both roles count matches independently, so both have to start a series
    // from zero. A join is always the start of one for this device: the lobby
    // closes for the whole series, so there is no way to arrive mid-way.
    _matchesPlayed = 0;
    _state = MP_CLIENT_LOBBY;
    return;
  }
  if (now - _waitStart > 1500){                                              // no ack
    _sys.audio.play(SFX_BACK);
    net().leave();
    _state = MP_BROWSE;
    return;
  }
  spinner(now, CRGB(120, 120, 0));
}

void Multiplayer::serviceClientLobby(uint32_t now){
  // Between rounds of a series already under way (matchesPlayed > 0) this is
  // the client's only stop -- the host skips straight back into beginMatch()
  // -- so it needs the same early-end as countdown/playing/result. Before a
  // series has played even one round this is just the ordinary post-join
  // wait, and B means leave, same as everywhere else pre-match.
  if (_matchesPlayed > 0 && _sys.buttonB.wasPressed() && tryEndSeriesEarly()) return;
  if (_sys.buttonB.wasPressed()){ _sys.audio.play(SFX_BACK); net().leave(); _state = MP_BROWSE; return; }
  if (net().msSinceHost(now) > 2000){                                        // host gone
    _sys.audio.play(SFX_BACK);
    net().leave();
    _state = MP_BROWSE;
    return;
  }

  // Checked before the started-without-us test below, because both can be true
  // on the same pass: the host broadcasts START and then beacons `started`, and
  // whichever this loop sees first, the START is the one that matters.
  StartPayload sp;
  if (net().takeStart(sp)){ startGame(sp.seed); return; }

  // MSG_START is a single unacked broadcast, and it is the one frame whose loss
  // strands us: the host plays on, and its beacons and STATE frames both keep
  // msSinceHost() fresh, so the host-gone timeout above never fires and this
  // spinner would run until somebody pressed B.
  //
  // A snapshot arriving while we are still in the lobby says it plainly -- a
  // match is running and we are not in it. This replaced a check on the lobby
  // beacon's `started` flag, which could not tell "started without me" from "has
  // not reopened since the last match yet" and so needed an arming step to
  // avoid breaking every rematch. Between series matches the host never reopens
  // at all, so that arming would never have happened and the protection would
  // have been silently absent for exactly the case it was written for.
  if (net().stateHeard()){
    _sys.audio.play(SFX_BACK);
    net().leave();
    _state = MP_BROWSE;
    return;
  }

  // The same roster the host is looking at. A spinner used to live here, which
  // told a joined player nothing at all -- not who else was in, not what they
  // had picked, not even which one was them.
  drawRoster(now);
  // ...but a still picture cannot say "still connected", which is what the
  // spinner was really for. One slow pulse in the corner carries that instead.
  const uint8_t v = (uint8_t)(((now / 8) % 120 < 60) ? 90 : 20);
  disp().setPixel(MATRIX_W - 1, MATRIX_H - 1, CRGB(0, v, v / 2));
  disp().show();
}

// ============================================================================
//  Countdown, play, result.
// ============================================================================
void Multiplayer::serviceCountdown(uint32_t now){
  if (_sys.buttonB.wasPressed() && tryEndSeriesEarly()) return;

  uint32_t elapsed = now - _cdStart;
  if (elapsed >= COUNTDOWN_MS){
    playMatchGo(_sys.audio);
    _lastTick = _lastInput = now;
    _tick = 0;
    _privHeld = false;
    // A lockstep tick consumes decisions made against a board that was already
    // published, so somebody has to publish the opening one. Without this the
    // first tick would run with every remote player idle -- harmless, but a
    // free tick nobody chose is exactly the sort of thing that goes unnoticed
    // and then explains a lost match.
    if (_game->lockstep() && net().role() == ROLE_HOST) publishBoard();
    _state = MP_PLAYING;
    return;
  }
  drawMatchCountdown(disp(), _sys.audio, elapsed);
}

void Multiplayer::servicePlaying(uint32_t now){
  if (_sys.buttonB.wasPressed() && tryEndSeriesEarly()) return;

  uint8_t ib[NET_INPUT_MAX];
  size_t  il;

  if (now - _lastInput >= INPUT_MS){
    LocalInput li = readInput();
    size_t n = _game->serializeInput(ib, sizeof(ib), li);
    if (net().role() == ROLE_HOST) net().submitInput(net().myId(), ib, n);
    else                           net().sendInput(ib, n);
    _lastInput = now;
  }

  if (net().role() == ROLE_HOST){
    if (now - _lastTick >= HOST_TICK_MS){
      for (uint8_t pid = 0; pid < net().matchPlayers(); pid++)
        if (net().getInput(pid, ib, il)) _game->applyInput(pid, ib, il);
      _game->hostTick();
      uint8_t sb[NET_STATE_MAX];
      size_t sn = _game->serializeState(sb, sizeof(sb));
      net().broadcastState(sb, sn, _tick++);
      _lastTick = now;
    }
  } else {
    uint8_t sb[NET_STATE_MAX];
    size_t  sn; uint16_t tk;
    if (net().takeState(sb, sn, tk)) _game->applyState(sb, sn);
    if (net().msSinceHost(now) > 2000){ net().leave(); _state = MP_BROWSE; return; }
  }

  uint8_t winner;
  if (_game->isOver(winner)) enterResult(now, winner);

  _game->render(disp());     // render() clears internally
  disp().show();
}

// ============================================================================
//  Lockstep play.
//
//  The inversion, in one sentence: instead of the host collecting control
//  readings and simulating everybody, it publishes a board and collects the
//  decisions that board produced on four different devices. It has to, because
//  each player's rule is compiled into their own unit and the host has no code
//  to run for anybody but itself.
//
//  _tick is the board everyone is currently thinking about -- the one the next
//  tick will read. The host publishes under that label, decisions come back
//  under it, and the host matches before applying. It is not bookkeeping: a
//  decision list has no cell indices in it, so a list applied to a board it was
//  not computed against lands its entries on the wrong cells entirely.
// ============================================================================
void Multiplayer::publishBoard(){
  uint8_t sb[NET_STATE_MAX];
  size_t  sn = _game->serializeState(sb, sizeof(sb));
  net().broadcastState(sb, sn, _tick);

  // Always sent, even when it comes back empty: a client waits for this frame
  // before it will decide, so "nothing to say" has to be said rather than left
  // indistinguishable from a frame that went missing.
  uint8_t pb[NET_PRIV_MAX];
  for (uint8_t pid = 0; pid < net().matchPlayers(); pid++){
    if (pid == net().myId()) continue;             // the host reads its own ledger
    size_t pn = _game->serializePrivate(pid, pb, sizeof(pb));
    net().sendPrivate(pid, pb, pn, _tick);
  }
}

void Multiplayer::servicePlayingLockstep(uint32_t now){
  if (_sys.buttonB.wasPressed() && tryEndSeriesEarly()) return;

  const uint16_t period = _game->tickMs() ? _game->tickMs() : HOST_TICK_MS;

  if (net().role() == ROLE_HOST){
    if (now - _lastTick >= period){
      // Take only what was decided against the board we published. Anything
      // else -- nothing arrived, it arrived late, it answers an older board --
      // is left out, and that player idles. One rule covers a dropped frame, a
      // rule that overran its deadline, and a unit that walked away.
      uint8_t  ib[NET_INPUT_MAX];
      size_t   il;
      uint16_t itick;
      for (uint8_t pid = 0; pid < net().matchPlayers(); pid++){
        if (pid == net().myId()) continue;         // our own rule runs inside hostTick
        if (net().getInput(pid, ib, il, itick) && itick == _tick)
          _game->applyInput(pid, ib, il);
      }

      // Our own rule runs inside hostTick too, so it never goes through
      // serializeInput -- this is the only way it sees this device's button.
      _game->setLocalInput(readInput());
      _game->hostTick();
      _tick++;
      publishBoard();
      _lastTick = now;
    }
  } else {
    // Hold the private frame until its board turns up; see the member comment.
    uint8_t  pb[NET_PRIV_MAX];
    size_t   pn;
    uint16_t ptick;
    if (net().takePrivate(pb, pn, ptick)){
      memcpy(_privBuf, pb, pn);
      _privLen = pn; _privTick = ptick; _privHeld = true;
    }

    uint8_t  sb[NET_STATE_MAX];
    size_t   sn;
    uint16_t stick;
    if (net().takeState(sb, sn, stick)){
      _game->applyState(sb, sn);
      // Energy first: a rule cannot answer canAfford() without it. If the
      // matching frame has not arrived, every cell reads zero and idles, which
      // costs this device one tick and disturbs nobody else.
      if (_privHeld && _privTick == stick){
        _game->applyPrivate(_privBuf, _privLen);
        _privHeld = false;
      }

      uint8_t db[NET_INPUT_MAX];
      size_t  dn = _game->serializeInput(db, sizeof(db), readInput());
      if (dn) net().sendInput(db, dn, stick);
    }

    if (net().msSinceHost(now) > 2000){ net().leave(); _state = MP_BROWSE; return; }
  }

  uint8_t winner;
  if (_game->isOver(winner)){
    _winner = winner;
    if (net().role() == ROLE_HOST && !_winRecorded && winner != 0xFF){
      net().recordWin(winner);
      _winRecorded = true;
    }
    _sys.audio.play(winner == net().myId() ? SFX_WIN : SFX_LOSE);
    _resultStart = now;
    _state = MP_RESULT;
  }

  _game->render(disp());     // render() clears internally
  disp().show();
}

void Multiplayer::serviceResult(uint32_t now){
  // The host can leave this screen before we do -- it only has to wait out the
  // lockout, and a press there sends START while we are still counting down our
  // own three seconds. Lingering is not merely cosmetic: the host's countdown
  // would run out and it would start ticking while we sat here, and every tick
  // we miss is one our virus plays as idle. So the moment the next match
  // exists, go to it.
  if (net().role() == ROLE_CLIENT){
    StartPayload sp;
    if (net().takeStart(sp)){ startGame(sp.seed); return; }
  }

  uint32_t el = now - _resultStart;
  if (matchResultShowsBoard(el)) _game->render(disp());  // final board, winner-colored border
  else                           drawScoreboard(now);    // then running standings
  disp().show();

  // Same lockout as the release check below, and for the same reason: the
  // press that just ended the match should not double as the press that ends
  // the series.
  if (el > MATCH_RESULT_LOCKOUT_MS && _sys.buttonB.wasPressed() && tryEndSeriesEarly()) return;

  bool pressed = _sys.buttonA.wasPressed() || _sys.buttonB.wasPressed();
  if (!matchResultReleased(el, pressed)) return;

  const uint8_t rounds = _game->seriesRounds();

  // A fixed-length session ends on its own, with standings, rather than
  // rematching forever. Both roles stop here: the host because it decides, the
  // client because it has counted the same matches and will not be sent
  // another START.
  if (rounds && _matchesPlayed >= rounds){ _state = MP_SERIES_OVER; return; }

  if (net().role() == ROLE_HOST){
    if (rounds){
      beginMatch();                    // next match of the series, straight away
    } else {
      net().reopen();                  // Tron: back to an open lobby
      _state = MP_HOST_LOBBY;
    }
  } else {
    net().clearStateHeard();           // the match we just played is not news
    _state = MP_CLIENT_LOBBY;          // await the host's next start
  }
}

// ============================================================================
//  Series over: who took the most matches. A is another series, B leaves.
// ============================================================================
void Multiplayer::serviceSeriesOver(uint32_t now){
  const int n = constrain((int)net().matchPlayers(), 1, NET_MAX_PLAYERS);

  uint8_t champ = 0xFF, best = 0;
  bool    tied  = false;
  uint8_t scores[NET_MAX_PLAYERS];
  CRGB    colors[NET_MAX_PLAYERS];
  for (int i = 0; i < n; i++){
    scores[i] = net().scoreOf(i);
    colors[i] = _game->playerColor(i);
    if (scores[i] > best){ best = scores[i]; champ = (uint8_t)i; tied = false; }
    else if (scores[i] == best && best) tied = true;
  }
  if (tied) champ = 0xFF;              // a drawn series frames nobody

  drawWinBars(disp(), scores, colors, n, champ, now);
  if (champ != 0xFF) disp().border(_game->playerColor(champ));
  disp().show();

  if (_sys.buttonB.wasPressed()){
    _sys.audio.play(SFX_BACK);
    net().leave();
    _wantExit = true;
    return;
  }

  if (net().role() == ROLE_HOST){
    if (_sys.buttonA.wasPressed()){
      _sys.audio.play(SFX_SELECT);
      net().resetScores();
      _matchesPlayed = 0;
      beginMatch();
    }
    return;
  }

  // Client: the host may start a fresh series from this same screen.
  StartPayload sp;
  if (net().takeStart(sp)){ _matchesPlayed = 0; startGame(sp.seed); return; }
  if (net().msSinceHost(now) > 2000){ net().leave(); _state = MP_BROWSE; }
}

// A rotating dot as a "waiting" indicator; the top row shows one pixel per
// connected player. Derived from the panel centre rather than the hard-coded
// 8x8 ring it was, so it stays in the middle of whatever it is drawn on.
void Multiplayer::spinner(uint32_t now, const CRGB& c){
  static const int8_t ox[8] = { 0, 1, 2, 2, 2, 1, 0, 0 };   // unit ring, 0..2
  static const int8_t oy[8] = { 0, 0, 0, 1, 2, 2, 2, 1 };
  const int r  = MATRIX_W / 5;                              // ring radius
  const int cx = MATRIX_W / 2 - r, cy = MATRIX_H / 2 - r;

  disp().clear();
  int i = (int)((now / 100) % 8);
  disp().setPixel(cx + ox[i] * r, cy + oy[i] * r, c);
  for (int k = 0; k < net().playerCount() && k < MATRIX_W; k++)
    disp().setPixel(k, 0, CRGB(0, 60, 60));
  disp().show();
}

// Running standings via the shared win-bar renderer.
void Multiplayer::drawScoreboard(uint32_t now){
  int n = constrain((int)net().matchPlayers(), 1, NET_MAX_PLAYERS);
  uint8_t scores[NET_MAX_PLAYERS];
  CRGB    colors[NET_MAX_PLAYERS];
  for (int i = 0; i < n; i++){ scores[i] = net().scoreOf(i); colors[i] = _game->playerColor(i); }
  drawWinBars(disp(), scores, colors, n, _winner, now);
}
