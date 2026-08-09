#include "net.h"
#include "core/storage.h"
#include "core/palette.h"
#include <esp_random.h>
#include <string.h>

#define BEACON_MS      250    // host lobby-announce interval
#define LOBBY_TIMEOUT  1500   // drop a discovered lobby unheard-from this long
// A player's input blob is worth replaying for a few missed frames -- inputs
// arrive every MATCH_INPUT_MS (40 ms) and one lost frame is normal -- but not
// forever. Twelve missed in a row is a unit that has gone away, not a glitch.
#define INPUT_TIMEOUT  500

// Every frame this file builds has to fit what the radio will carry. The size
// constants were derived by hand against a 250-byte cap and a 10-byte header,
// and two of them grew for lockstep play, so the derivation is pinned here
// rather than left in a comment to rot.
static_assert(sizeof(NetHeader) == 10, "NetHeader grew; every payload budget was derived from 10");
static_assert(sizeof(NetHeader) + NET_STATE_MAX <= NET_MAX_PAYLOAD, "STATE frame exceeds the ESP-NOW cap");
static_assert(sizeof(NetHeader) + NET_INPUT_MAX <= NET_MAX_PAYLOAD, "INPUT frame exceeds the ESP-NOW cap");
static_assert(sizeof(NetHeader) + NET_PRIV_MAX  <= NET_MAX_PAYLOAD, "PRIVSTATE frame exceeds the ESP-NOW cap");
static_assert(sizeof(NetHeader) + NET_CTRL_MAX  <= NET_MAX_PAYLOAD, "control frame exceeds the ESP-NOW cap");

// Every control payload has to fit the frame sendCtrl builds on the stack.
// These are what make NET_CTRL_MAX a guarantee rather than a hope.
static_assert(sizeof(LobbyPayload)   <= NET_CTRL_MAX, "LobbyPayload exceeds NET_CTRL_MAX");
static_assert(sizeof(JoinPayload)    <= NET_CTRL_MAX, "JoinPayload exceeds NET_CTRL_MAX");
static_assert(sizeof(JoinAckPayload) <= NET_CTRL_MAX, "JoinAckPayload exceeds NET_CTRL_MAX");
static_assert(sizeof(StartPayload)   <= NET_CTRL_MAX, "StartPayload exceeds NET_CTRL_MAX");
static_assert(sizeof(ScoresPayload)  <= NET_CTRL_MAX, "ScoresPayload exceeds NET_CTRL_MAX");

#define NS_NET    "net"       // storage namespace
#define KEY_COLOR "color"     // this device's color preference (preset index)

void Net::begin(Storage* storage){
  bool ok = _link.begin(NET_CHANNEL);
  const uint8_t* m = _link.mac();

  _storage = storage;
  uint8_t defColor = (uint8_t)(m[5] % NUM_PRESET_COLORS);   // auto variance from MAC
  _myColorIndex = _storage ? (uint8_t)_storage->getU32(NS_NET, KEY_COLOR, defColor) : defColor;
  if (_myColorIndex >= NUM_PRESET_COLORS) _myColorIndex = 0;

  Serial.printf("[NET] ESP-NOW %s  MAC %02X:%02X:%02X:%02X:%02X:%02X  ch %d  color %u\n",
                ok ? "up" : "FAIL", m[0], m[1], m[2], m[3], m[4], m[5], NET_CHANNEL, _myColorIndex);
  resetSession();
}

void Net::setColorIndex(uint8_t idx){
  if (idx >= NUM_PRESET_COLORS) idx = 0;
  _myColorIndex = idx;
  if (_storage) _storage->putU32(NS_NET, KEY_COLOR, idx);
  if (_role == ROLE_HOST) _colorIndex[0] = idx;
}

void Net::resetSession(){
  _role = ROLE_NONE;
  _session = 0;
  _gameId = _maxPlayers = _arenaW = _arenaH = 0;
  _myId = NET_PID_NONE;
  _playerCount = _matchPlayers = 0;
  _started = false;
  _stateHeard = false;
  for (int i = 0; i < NET_MAX_PLAYERS; i++){
    _slotUsed[i] = false; _input[i].len = 0; _input[i].at = 0; _input[i].tick = 0;
    _colorIndex[i] = 0; _tag[i] = 0; _colors[i] = CRGB::White; _score[i] = 0;
  }
  _state.fresh = false;
  _state.seen  = false;
  _state.tick  = 0;
  _priv.fresh  = false;
  _priv.len    = 0;
  _joinAckPending = _startPending = false;
  // _browseGame survives on purpose -- it describes what this device is looking
  // for, not what session it is in.
}

void Net::fillHeader(NetHeader& h, uint8_t type){
  h.magic   = NET_MAGIC;
  h.ver     = NET_PROTO_VER;
  h.type    = type;
  h.gameId  = _gameId;
  h.session = _session;
  h.sender  = _myId;
  h.tick    = 0;
}

void Net::sendCtrl(const uint8_t* mac, uint8_t type, const void* payload, size_t plen){
  uint8_t out[sizeof(NetHeader) + NET_CTRL_MAX];
  NetHeader h; fillHeader(h, type);
  memcpy(out, &h, sizeof(NetHeader));
  if (plen) memcpy(out + sizeof(NetHeader), payload, plen);
  if (mac) _link.unicast(mac, out, sizeof(NetHeader) + plen);
  else     _link.broadcast(out, sizeof(NetHeader) + plen);
}

// ============================================================================
//  Main pump: drain the RX queue, run the host beacon, prune stale lobbies.
// ============================================================================
void Net::update(uint32_t now){
  RxFrame f;
  while (_link.poll(f)) handleFrame(f, now);

  if (_role == ROLE_HOST && now - _lastBeacon >= BEACON_MS){
    LobbyPayload lp{};
    lp.gameId = _gameId; lp.arenaW = _arenaW; lp.arenaH = _arenaH;
    lp.playerCount = _playerCount; lp.maxPlayers = _maxPlayers;
    lp.started = (uint8_t)(_started ? 1 : 0);
    lp.slots = 0;
    for (uint8_t i = 0; i < NET_MAX_PLAYERS; i++) if (_slotUsed[i]) lp.slots |= (uint8_t)(1u << i);
    // The whole roster, every beacon. It is 10 bytes on a frame that has 240
    // spare and it goes out four times a second either way, so publishing the
    // full picture costs nothing next to inventing a "roster changed" message
    // whose loss would leave a client showing a lobby that no longer exists.
    for (uint8_t i = 0; i < NET_MAX_PLAYERS; i++){
      lp.colorIndex[i] = _slotUsed[i] ? _colorIndex[i] : 0;
      lp.tag[i]        = _slotUsed[i] ? _tag[i] : 0;
    }
    sendCtrl(nullptr, MSG_LOBBY, &lp, sizeof(lp));
    _lastBeacon = now;
  }

  for (auto& l : _lobbies)
    if (l.used && now - l.lastSeen > LOBBY_TIMEOUT) l.used = false;

  // Expire input from players who have gone quiet mid-match, so getInput()
  // stops handing the sim a stale blob as though it were this tick's. Slot
  // _myId is the host's own and is refilled locally every tick, so it is never
  // stale; skipping it also keeps a host from expiring itself.
  if (_role == ROLE_HOST && _started){
    for (uint8_t i = 0; i < NET_MAX_PLAYERS; i++){
      if (i == _myId || !_input[i].len) continue;
      if (now - _input[i].at > INPUT_TIMEOUT) _input[i].len = 0;
    }
  }
}

// ============================================================================
//  Inbound frame dispatch.
// ============================================================================
void Net::handleFrame(const RxFrame& f, uint32_t now){
  if (f.len < sizeof(NetHeader)) return;
  NetHeader h;
  memcpy(&h, f.data, sizeof(NetHeader));
  if (h.magic != NET_MAGIC || h.ver != NET_PROTO_VER) return;

  // "Stable id; only matching games talk" is what net_game.h has promised since
  // the interface was written, and until now nothing anywhere compared the
  // field -- the same shape of bug as the arena dims, which were carried,
  // documented as enforced, and ignored. Discovery is the one exception: a
  // browser has to hear every host on the channel in order to filter the list
  // (see openLobbyAt). Everything else is session traffic and has to be for the
  // game we are actually in. When we are in no session _gameId is 0, which no
  // real game uses, so this also drops stray session frames while browsing.
  if (h.type != MSG_LOBBY && h.gameId != _gameId) return;

  const uint8_t* pl = f.data + sizeof(NetHeader);
  size_t plen = f.len - sizeof(NetHeader);
  bool fromHost = (_role == ROLE_CLIENT && memcmp(f.src, _hostMac, 6) == 0);
  if (fromHost) _lastHostMsg = now;

  switch (h.type){

    case MSG_LOBBY: {
      if (plen < sizeof(LobbyPayload)) break;
      LobbyPayload lp; memcpy(&lp, pl, sizeof(lp));
      int i = findLobby(f.src);
      if (i < 0){ for (int k = 0; k < (int)(sizeof(_lobbies)/sizeof(_lobbies[0])); k++) if (!_lobbies[k].used){ i = k; break; } }
      if (i >= 0){
        LobbyInfo& L = _lobbies[i];
        memcpy(L.hostMac, f.src, 6);
        L.gameId = lp.gameId; L.arenaW = lp.arenaW; L.arenaH = lp.arenaH;
        L.playerCount = lp.playerCount; L.maxPlayers = lp.maxPlayers;
        L.started = lp.started; L.colorIndex = lp.colorIndex[0];   // slot 0 is the host
        L.lastSeen = now; L.used = true;
      }
      if (fromHost){
        _playerCount = lp.playerCount;
        // A client's entire knowledge of who else is here. Adopted only from
        // OUR host: every open lobby on the channel beacons, and taking a
        // roster from the wrong one would paint somebody else's players.
        for (uint8_t i = 0; i < NET_MAX_PLAYERS; i++){
          _slotUsed[i]   = (lp.slots & (1u << i)) != 0;
          _colorIndex[i] = lp.colorIndex[i];
          _tag[i]        = lp.tag[i];
        }
      }
      break;
    }

    case MSG_JOIN: {
      if (_role != ROLE_HOST || _started || plen < sizeof(JoinPayload)) break;
      JoinPayload jp; memcpy(&jp, pl, sizeof(jp));
      int pid = addRoster(f.src);
      if (pid < 0) break;                       // full
      _colorIndex[pid] = jp.colorIndex;
      _tag[pid]        = jp.tag;
      _link.addPeer(f.src);
      JoinAckPayload ap{ (uint8_t)pid, _gameId, _arenaW, _arenaH };
      sendCtrl(f.src, MSG_JOINACK, &ap, sizeof(ap));
      recountPlayers();
      broadcastScores(f.src);                   // bring the newcomer up to date
      break;
    }

    case MSG_JOINACK: {
      if (_role != ROLE_CLIENT || !fromHost || plen < sizeof(JoinAckPayload)) break;
      JoinAckPayload ap; memcpy(&ap, pl, sizeof(ap));
      _myId    = ap.playerId;
      _session = h.session;
      _gameId  = ap.gameId;
      _arenaW  = ap.arenaW;
      _arenaH  = ap.arenaH;
      _joinAckPending = true;
      break;
    }

    case MSG_LEAVE: {
      if (_role == ROLE_HOST){
        for (int i = 1; i < NET_MAX_PLAYERS; i++)
          if (_slotUsed[i] && memcmp(_roster[i], f.src, 6) == 0){ _slotUsed[i] = false; _input[i].len = 0; }
        recountPlayers();
      }
      break;
    }

    case MSG_START: {
      if (_role != ROLE_CLIENT || !fromHost || plen < sizeof(StartPayload)) break;
      memcpy(&_startMsg, pl, sizeof(StartPayload));
      _arenaW = _startMsg.arenaW; _arenaH = _startMsg.arenaH;
      _matchPlayers = _startMsg.numPlayers;
      for (int i = 0; i < NET_MAX_PLAYERS; i++)                // adopt resolved palette
        _colors[i] = CRGB(_startMsg.colors[i*3], _startMsg.colors[i*3+1], _startMsg.colors[i*3+2]);
      _startPending = true;
      // A new match restarts the host's tick counter at 0, so the snapshot
      // ordering below has to forget the last match's high-water mark or it
      // would reject every frame of this one.
      _state.fresh = false; _state.seen = false; _state.tick = 0;
      _stateHeard  = false;   // this START is ours; we are not being left behind
      break;
    }

    case MSG_INPUT: {
      if (_role != ROLE_HOST || h.session != _session) break;
      uint8_t pid = h.sender;
      if (pid >= NET_MAX_PLAYERS || !_slotUsed[pid]) break;
      uint8_t n = plen > NET_INPUT_MAX ? NET_INPUT_MAX : (uint8_t)plen;
      memcpy(_input[pid].buf, pl, n);
      _input[pid].len  = n;
      _input[pid].at   = now;
      _input[pid].tick = h.tick;
      break;
    }

    case MSG_PRIVSTATE: {
      if (_role != ROLE_CLIENT || !fromHost || h.session != _session) break;
      uint8_t n = plen > NET_PRIV_MAX ? NET_PRIV_MAX : (uint8_t)plen;
      memcpy(_priv.buf, pl, n);
      _priv.len = n; _priv.tick = h.tick; _priv.fresh = true;
      break;
    }

    case MSG_STATE: {
      if (_role != ROLE_CLIENT || !fromHost || h.session != _session) break;
      // Noted before the ordering filter below: this says a match is RUNNING,
      // which is true of an out-of-order frame just as much as a fresh one.
      _stateHeard = true;
      // Snapshots are whole-world and self-healing, so a lost one costs
      // nothing -- but an out-of-order one costs a visible rewind. Signed
      // difference so the comparison survives the tick counter wrapping.
      if (_state.seen && (int16_t)(h.tick - _state.tick) <= 0) break;
      uint8_t n = plen > NET_STATE_MAX ? NET_STATE_MAX : (uint8_t)plen;
      memcpy(_state.buf, pl, n);
      _state.len = n; _state.tick = h.tick; _state.fresh = true; _state.seen = true;
      break;
    }

    case MSG_SCORES: {
      if (_role != ROLE_CLIENT || !fromHost || plen < 1) break;
      ScoresPayload sp; memcpy(&sp, pl, sizeof(sp) < plen ? sizeof(sp) : plen);
      uint8_t n = sp.numPlayers > NET_MAX_PLAYERS ? NET_MAX_PLAYERS : sp.numPlayers;
      for (uint8_t i = 0; i < n; i++) _score[i] = sp.score[i];
      break;
    }
  }
}

// ============================================================================
//  Browse
// ============================================================================
int Net::findLobby(const uint8_t* mac){
  for (int i = 0; i < (int)(sizeof(_lobbies)/sizeof(_lobbies[0])); i++)
    if (_lobbies[i].used && memcmp(_lobbies[i].hostMac, mac, 6) == 0) return i;
  return -1;
}

// Both list only lobbies running the game the runner asked for. Discovery
// stores everything it hears -- filtering on receipt would mean a browser could
// never be re-pointed at a different game without losing what it already knew --
// so the game test lives here, at the point the list is presented.
bool Net::lobbyVisible(const LobbyInfo& l) const {
  return l.used && !l.started && l.gameId == _browseGame;
}

int Net::openLobbyCount() const {
  int n = 0;
  for (auto& l : _lobbies) if (lobbyVisible(l)) n++;
  return n;
}

const LobbyInfo* Net::openLobbyAt(int visibleIndex) const {
  int n = 0;
  for (auto& l : _lobbies)
    if (lobbyVisible(l)){ if (n == visibleIndex) return &l; n++; }
  return nullptr;
}

// ============================================================================
//  Session control
// ============================================================================
void Net::host(uint8_t gameId, uint8_t maxPlayers, uint8_t arenaW, uint8_t arenaH){
  resetSession();
  _role = ROLE_HOST;
  _session = (uint16_t)esp_random();
  _gameId = gameId; _maxPlayers = maxPlayers; _arenaW = arenaW; _arenaH = arenaH;
  _myId = 0;
  memcpy(_roster[0], _link.mac(), 6);
  _slotUsed[0] = true;
  _colorIndex[0] = _myColorIndex;     // host is player 0
  _tag[0]        = _myTag;
  recountPlayers();
  _lastBeacon = 0;                    // announce immediately on next update()
}

bool Net::join(const LobbyInfo& l){
  resetSession();
  _role = ROLE_CLIENT;
  memcpy(_hostMac, l.hostMac, 6);
  _gameId = l.gameId; _arenaW = l.arenaW; _arenaH = l.arenaH; _maxPlayers = l.maxPlayers;
  _myId = NET_PID_NONE;
  _link.addPeer(l.hostMac);
  JoinPayload jp;
  memcpy(jp.mac, _link.mac(), 6);
  jp.colorIndex = _myColorIndex;
  jp.tag        = _myTag;
  sendCtrl(_hostMac, MSG_JOIN, &jp, sizeof(jp));
  return true;
}

void Net::leave(){
  if (_role == ROLE_CLIENT) sendCtrl(_hostMac, MSG_LEAVE, nullptr, 0);
  resetSession();
}

void Net::startMatch(uint32_t seed){
  if (_role != ROLE_HOST) return;
  _started = true;
  _matchPlayers = _playerCount;
  resolvePalette();                                // dedup colors before we send them

  StartPayload sp{};
  sp.gameId = _gameId; sp.arenaW = _arenaW; sp.arenaH = _arenaH;
  sp.numPlayers = _playerCount; sp.seed = seed;
  for (int i = 0; i < NET_MAX_PLAYERS; i++){
    sp.colors[i*3]   = _colors[i].r;
    sp.colors[i*3+1] = _colors[i].g;
    sp.colors[i*3+2] = _colors[i].b;
  }
  sendCtrl(nullptr, MSG_START, &sp, sizeof(sp));   // fan out to clients
}

// Next-free-preset dedup: players resolved in id order keep their chosen preset
// if still free, else take the next unused one. With 5 presets and <=5 players
// this always yields distinct, clear colors.
void Net::resolvePalette(){
  uint8_t n = _matchPlayers ? _matchPlayers : _playerCount;
  if (n > NET_MAX_PLAYERS) n = NET_MAX_PLAYERS;
  bool used[NUM_PRESET_COLORS] = {false};
  int  idx[NET_MAX_PLAYERS];
  for (int i = 0; i < n; i++) idx[i] = -1;
  for (int i = 0; i < n; i++){                      // keep chosen preset if free
    uint8_t c = _colorIndex[i];
    if (c < NUM_PRESET_COLORS && !used[c]){ used[c] = true; idx[i] = c; }
  }
  for (int i = 0; i < n; i++){                       // clashes bump to next free
    if (idx[i] >= 0) continue;
    for (int k = 0; k < NUM_PRESET_COLORS; k++) if (!used[k]){ used[k] = true; idx[i] = k; break; }
    if (idx[i] < 0) idx[i] = 0;
  }
  for (int i = 0; i < n; i++) _colors[i] = PRESET_COLORS[idx[i]];
}

void Net::reopen(){
  if (_role != ROLE_HOST) return;
  _started = false;                  // roster + session kept for a quick rematch
}

void Net::recountPlayers(){
  uint8_t n = 0;
  for (int i = 0; i < NET_MAX_PLAYERS; i++) if (_slotUsed[i]) n++;
  _playerCount = n;
}

int Net::addRoster(const uint8_t* mac){
  for (int i = 1; i < NET_MAX_PLAYERS; i++)
    if (_slotUsed[i] && memcmp(_roster[i], mac, 6) == 0) return i;   // already in
  for (int i = 1; i < _maxPlayers && i < NET_MAX_PLAYERS; i++)
    if (!_slotUsed[i]){ memcpy(_roster[i], mac, 6); _slotUsed[i] = true; return i; }
  return -1;
}

// ============================================================================
//  Latched events
// ============================================================================
bool Net::takeJoinAck(){ bool v = _joinAckPending; _joinAckPending = false; return v; }

bool Net::takeStart(StartPayload& out){
  if (!_startPending) return false;
  out = _startMsg; _startPending = false; return true;
}

// ============================================================================
//  Match data plane
// ============================================================================
void Net::submitInput(uint8_t pid, const uint8_t* buf, size_t len, uint16_t tick){
  if (pid >= NET_MAX_PLAYERS) return;
  uint8_t n = len > NET_INPUT_MAX ? NET_INPUT_MAX : (uint8_t)len;
  memcpy(_input[pid].buf, buf, n);
  _input[pid].len  = n;
  _input[pid].at   = millis();
  _input[pid].tick = tick;
}

void Net::sendInput(const uint8_t* buf, size_t len, uint16_t tick){
  if (_role != ROLE_CLIENT || _myId == NET_PID_NONE) return;
  uint8_t out[sizeof(NetHeader) + NET_INPUT_MAX];
  NetHeader h; fillHeader(h, MSG_INPUT); h.tick = tick;
  uint8_t n = len > NET_INPUT_MAX ? NET_INPUT_MAX : (uint8_t)len;
  memcpy(out, &h, sizeof(NetHeader));
  memcpy(out + sizeof(NetHeader), buf, n);
  _link.unicast(_hostMac, out, sizeof(NetHeader) + n);
}

bool Net::getInput(uint8_t pid, uint8_t* buf, size_t& len){
  uint16_t ignored;
  return getInput(pid, buf, len, ignored);
}

bool Net::getInput(uint8_t pid, uint8_t* buf, size_t& len, uint16_t& tick){
  if (pid >= NET_MAX_PLAYERS || _input[pid].len == 0) return false;
  len  = _input[pid].len;
  tick = _input[pid].tick;
  memcpy(buf, _input[pid].buf, len);
  return true;
}

// Unicast to one player's roster MAC. Slot 0 is the host itself and has no peer
// to send to, so it is skipped along with empty slots -- letting a host loop
// over every player without special-casing itself is the whole point.
void Net::sendPrivate(uint8_t pid, const uint8_t* buf, size_t len, uint16_t tick){
  if (_role != ROLE_HOST || pid == _myId) return;
  if (pid >= NET_MAX_PLAYERS || !_slotUsed[pid]) return;
  uint8_t out[sizeof(NetHeader) + NET_PRIV_MAX];
  NetHeader h; fillHeader(h, MSG_PRIVSTATE); h.tick = tick;
  uint8_t n = len > NET_PRIV_MAX ? NET_PRIV_MAX : (uint8_t)len;
  memcpy(out, &h, sizeof(NetHeader));
  memcpy(out + sizeof(NetHeader), buf, n);
  _link.unicast(_roster[pid], out, sizeof(NetHeader) + n);
}

bool Net::takePrivate(uint8_t* buf, size_t& len, uint16_t& tick){
  if (!_priv.fresh) return false;
  len = _priv.len; tick = _priv.tick;
  memcpy(buf, _priv.buf, len);
  _priv.fresh = false;
  return true;
}

void Net::broadcastState(const uint8_t* buf, size_t len, uint16_t tick){
  if (_role != ROLE_HOST) return;
  uint8_t out[sizeof(NetHeader) + NET_STATE_MAX];
  NetHeader h; fillHeader(h, MSG_STATE); h.tick = tick;
  uint8_t n = len > NET_STATE_MAX ? NET_STATE_MAX : (uint8_t)len;
  memcpy(out, &h, sizeof(NetHeader));
  memcpy(out + sizeof(NetHeader), buf, n);
  _link.broadcast(out, sizeof(NetHeader) + n);
}

bool Net::takeState(uint8_t* buf, size_t& len, uint16_t& tick){
  if (!_state.fresh) return false;
  len = _state.len; tick = _state.tick;
  memcpy(buf, _state.buf, len);
  _state.fresh = false;
  return true;
}

// ============================================================================
//  Scoreboard
// ============================================================================
void Net::resetScores(){
  if (_role != ROLE_HOST) return;
  for (int i = 0; i < NET_MAX_PLAYERS; i++) _score[i] = 0;
  broadcastScores(nullptr);
}

void Net::recordWin(uint8_t pid){
  if (_role != ROLE_HOST || pid >= NET_MAX_PLAYERS) return;
  _score[pid]++;
  broadcastScores(nullptr);
}

void Net::recordTeamWin(){
  if (_role != ROLE_HOST) return;
  uint8_t n = _matchPlayers ? _matchPlayers : _playerCount;
  if (n > NET_MAX_PLAYERS) n = NET_MAX_PLAYERS;
  for (uint8_t i = 0; i < n; i++) _score[i]++;
  broadcastScores(nullptr);
}

void Net::broadcastScores(const uint8_t* mac){
  if (_role != ROLE_HOST) return;
  uint8_t n = _matchPlayers ? _matchPlayers : _playerCount;
  if (n > NET_MAX_PLAYERS) n = NET_MAX_PLAYERS;
  ScoresPayload sp{};
  sp.numPlayers = n;
  for (uint8_t i = 0; i < n; i++) sp.score[i] = _score[i];
  sendCtrl(mac, MSG_SCORES, &sp, sizeof(sp));
}
