#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include "net_link.h"
#include "net_proto.h"

class Storage;   // for persisting this device's color preference

// ============================================================================
//  Layers 2 & 3 -- Session/Lobby + Message/Sync. Game-agnostic. Owns device
//  identity, discovery, the host roster, and the match data plane (per-player
//  input up, state snapshots down). Sits on NetLink; knows nothing about any
//  specific game beyond its gameId.
//
//  Lives on System like Display/Storage. Single instance per device.
// ============================================================================

enum NetRole { ROLE_NONE, ROLE_HOST, ROLE_CLIENT };

// One discovered open lobby (built from MSG_LOBBY beacons while browsing).
struct LobbyInfo {
  uint8_t  hostMac[6];
  uint8_t  gameId;
  uint8_t  arenaW, arenaH;
  uint8_t  playerCount;
  uint8_t  maxPlayers;
  uint8_t  started;
  uint8_t  colorIndex;      // host's chosen color (browse swatch)
  uint32_t lastSeen;
  bool     used;
};

class Net {
public:
  void begin(Storage* storage = nullptr);
  void update(uint32_t now);          // drain RX, host beacon, prune stale lobbies

  // ---- device identity / color (persisted) ----
  uint8_t myColorIndex() const { return _myColorIndex; }
  void    setColorIndex(uint8_t idx);   // updates + persists
  uint8_t colorIndexOf(uint8_t pid) const { return pid < NET_MAX_PLAYERS ? _colorIndex[pid] : 0; }

  // ---- lobby tag: one game-defined byte per player, carried not interpreted --
  // Set before host()/join() -- it travels with JOIN and then rides the host's
  // beacon, so it is a property of the session rather than something re-sent.
  // Not persisted: unlike the colour, this is whatever the current game means
  // by it, and the game owns where it is stored (see NetGame::lobbyTag()).
  void    setLocalTag(uint8_t t) { _myTag = t; }
  uint8_t myTag() const { return _myTag; }
  uint8_t tagOf(uint8_t pid) const { return pid < NET_MAX_PLAYERS ? _tag[pid] : 0; }

  // Is this playerId occupied? Anything drawing a roster must ask this rather
  // than walking 0..playerCount-1: the count is a count, and a departure in the
  // middle leaves a hole. Valid on a client too -- the host's beacon carries
  // the occupancy mask alongside the roster.
  bool    slotUsed(uint8_t pid) const { return pid < NET_MAX_PLAYERS && _slotUsed[pid]; }

  // ---- resolved player palette (valid once a match has started) ----
  CRGB        colorOf(uint8_t pid) const { return pid < NET_MAX_PLAYERS ? _colors[pid] : CRGB::White; }
  const CRGB* colors() const { return _colors; }

  // ---- scoreboard ----
  void    recordWin(uint8_t pid);     // host: bump a player's wins, sync to all
  // Host: everybody's bar goes up. A co-op game has no single winner to credit
  // (NetGame::teamGame()), and calling recordWin() in a loop would put one
  // redundant broadcast on the air per player for a board that changed once.
  void    recordTeamWin();
  uint8_t scoreOf(uint8_t pid) const { return pid < NET_MAX_PLAYERS ? _score[pid] : 0; }

  // ---- browse ----
  // Which game's lobbies to list. Discovery hears every host on the channel,
  // but a runner can only play the one game it was built around, so the list is
  // filtered. Set by the runner before browsing. Deliberately NOT cleared by
  // leave()/resetSession(): it is a browse preference, not session state, and
  // backing out of a lobby must not empty the list you backed out into.
  void browseFor(uint8_t gameId) { _browseGame = gameId; }

  int              openLobbyCount() const;
  const LobbyInfo* openLobbyAt(int visibleIndex) const;   // among matching open lobbies

  // Has a STATE frame for our session arrived since the last START? A match is
  // running, and if we are still sitting in the lobby it started without us --
  // which is the only way to notice that a dropped MSG_START left us behind.
  //
  // Recorded BEFORE the snapshot-ordering filter, deliberately. The question
  // here is whether a match exists, not whether that particular snapshot was
  // worth applying -- and after a rematch the host's tick restarts at 0, so
  // every frame fails the ordering test. That is exactly when this has to work.
  bool stateHeard() const { return _stateHeard; }
  void clearStateHeard()  { _stateHeard = false; }

  // Host: wipe the standings and tell everyone. Starting a fresh series.
  void resetScores();

  // ---- session control ----
  void host(uint8_t gameId, uint8_t maxPlayers, uint8_t arenaW, uint8_t arenaH);
  bool join(const LobbyInfo& l);
  void leave();
  void startMatch(uint32_t seed);     // host only: broadcast START
  void reopen();                      // host only: back to an open lobby after a match

  NetRole role()         const { return _role; }
  uint8_t myId()         const { return _myId; }
  uint8_t playerCount()  const { return _playerCount; }
  uint8_t matchPlayers() const { return _matchPlayers; }
  uint8_t gameId()       const { return _gameId; }
  uint8_t arenaW()       const { return _arenaW; }
  uint8_t arenaH()       const { return _arenaH; }
  uint32_t msSinceHost(uint32_t now) const { return now - _lastHostMsg; }

  // ---- latched events (each consumed once) ----
  bool takeJoinAck();
  bool takeStart(StartPayload& out);

  // ---- match data plane ----
  // `tick` on the input path is the tick the sender computed against. A game
  // that streams control readings leaves it 0 and never looks; a lockstep game
  // has to match it before applying, because its blob is positionally indexed
  // against one specific board (see net_proto.h).
  void submitInput(uint8_t pid, const uint8_t* buf, size_t len, uint16_t tick = 0);
  void sendInput(const uint8_t* buf, size_t len, uint16_t tick = 0);   // client -> host
  bool getInput(uint8_t pid, uint8_t* buf, size_t& len);               // host: latest for pid
  bool getInput(uint8_t pid, uint8_t* buf, size_t& len, uint16_t& tick);
  void broadcastState(const uint8_t* buf, size_t len, uint16_t tick);
  bool takeState(uint8_t* buf, size_t& len, uint16_t& tick);      // client: newest unread

  // Per-player private state, host -> that player alone. Silently does nothing
  // for a pid that is not a live remote slot, so a host can loop over every
  // player without special-casing itself.
  void sendPrivate(uint8_t pid, const uint8_t* buf, size_t len, uint16_t tick);
  bool takePrivate(uint8_t* buf, size_t& len, uint16_t& tick);    // client: newest unread

private:
  NetLink  _link;
  NetRole  _role        = ROLE_NONE;
  uint16_t _session     = 0;
  uint8_t  _gameId      = 0, _maxPlayers = 0, _arenaW = 0, _arenaH = 0;
  uint8_t  _myId        = NET_PID_NONE;
  uint8_t  _playerCount = 0;
  uint8_t  _matchPlayers= 0;
  bool     _started     = false;

  // host roster: MAC per playerId (slot 0 = self/host)
  uint8_t  _roster[NET_MAX_PLAYERS][6];
  bool     _slotUsed[NET_MAX_PLAYERS] = {false};

  uint8_t  _hostMac[6]  = {0};        // client: our host
  uint32_t _lastHostMsg = 0;

  // identity + scoreboard (indexed by playerId)
  Storage* _storage = nullptr;
  uint8_t  _myColorIndex = 0;
  uint8_t  _myTag = 0;
  uint8_t  _colorIndex[NET_MAX_PLAYERS] = {0};   // each player's chosen preset
  // Roster tags. On a host these arrive with each JOIN; on a client they arrive
  // wholesale in the host's beacon, which is the only way a client learns
  // anything at all about its fellow players before MSG_START.
  uint8_t  _tag[NET_MAX_PLAYERS] = {0};
  CRGB     _colors[NET_MAX_PLAYERS];             // host-resolved render palette
  uint8_t  _score[NET_MAX_PLAYERS] = {0};

  LobbyInfo _lobbies[NET_MAX_PLAYERS * 2];
  uint8_t   _browseGame  = 0;    // gameId the runner wants to see; 0 lists nothing
  bool      _stateHeard  = false;

  // `at` is when this blob last arrived. A client that goes silent mid-match
  // used to keep its final input replayed into the sim every tick forever; the
  // slot now expires so getInput() stops claiming to have something current.
  struct InputSlot { uint8_t buf[NET_INPUT_MAX]; uint8_t len; uint32_t at; uint16_t tick; };
  InputSlot _input[NET_MAX_PLAYERS] = {};

  // Client: the newest private state the host sent us. Single slot and newest
  // wins, exactly like _state -- it describes one tick, and a superseded one is
  // of no use to anybody.
  struct { uint8_t buf[NET_PRIV_MAX]; uint8_t len; uint16_t tick; bool fresh; } _priv = {};

  // `seen` gates the tick comparison: the first snapshot of a match is tick 0,
  // which would lose the ordering test against the previous match's last tick.
  struct { uint8_t buf[NET_STATE_MAX]; uint8_t len; uint16_t tick; bool fresh; bool seen; } _state = {};

  bool         _joinAckPending = false;
  bool         _startPending   = false;
  StartPayload _startMsg       = {};

  uint32_t _lastBeacon = 0;

  bool lobbyVisible(const LobbyInfo& l) const;   // open, live, and our game
  void resetSession();
  void fillHeader(NetHeader& h, uint8_t type);
  void sendCtrl(const uint8_t* mac, uint8_t type, const void* payload, size_t plen);
  void handleFrame(const RxFrame& f, uint32_t now);
  int  findLobby(const uint8_t* mac);
  int  addRoster(const uint8_t* mac);   // playerId, or -1 if full
  void recountPlayers();
  void broadcastScores(const uint8_t* mac);   // mac == nullptr -> broadcast
  void resolvePalette();                      // host: dedup colors, next-free-preset
};
