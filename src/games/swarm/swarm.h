#pragma once
#include "net/net_game.h"
#include "net/net_proto.h"      // NET_MAX_PLAYERS, NET_PID_NONE
#include "audio/audio.h"
#include "swarm_sim.h"

// ============================================================================
//  Swarm: a co-operative wave shooter, and the first game on this device the
//  players win or lose together. Up to four ships hold a band at the bottom of
//  the panel, six waves descend, and a boss closes the run. Everyone shares one
//  pool of three lives, and there is no friendly fire.
//
//  This class is the NetGame shell only -- every rule lives in SwarmSim, which
//  is Arduino-free and native-tested (see test/test_swarm). What is HERE is the
//  part a test cannot hold: the wire format for input, the render, the HUD row,
//  and the mapping from the sim's events onto the shared sfx catalog.
//
//  ---- Why the weapon rides in the input blob --------------------------------
//  Each player picks one of five weapons in the Swarm submenu. That choice has
//  to reach the host, and the obvious home for it -- JoinPayload, beside
//  colorIndex -- was rejected: it is a game-specific concept, it would cost a
//  NET_PROTO_VER bump, and it would push StartPayload to 28 of NET_CTRL_MAX's
//  32 bytes. The input blob is already opaque to the backend and already sent
//  25 times a second, so the weapon travels there for free and the protocol
//  does not move at all. The sim adopts a CHANGED weapon only at a wave break.
// ============================================================================

#define SWM_HUD_ROWS  1        // bottom panel row: lives, wave pips, boss health

class Swarm : public NetGame {
public:
  // The piezo is optional so the game can be constructed without a System --
  // audio is presentation, and nothing about the rules depends on it.
  explicit Swarm(Audio* audio = nullptr) : _audio(audio) {}

  uint8_t gameId()     const override { return 3; }   // 1 = Tron, 2 = Virus
  uint8_t maxPlayers() const override { return SWM_MAX_PLAYERS; }
  Icon    menuIcon()   const override;
  CRGB    playerColor(uint8_t playerId) const override;

  // The HUD row is not part of the board. Asking rather than assuming is what
  // keeps the number the lobby advertises the same as the number the game
  // draws on -- see NetGame::arenaH().
  uint8_t arenaH() const override { return MATRIX_H - SWM_HUD_ROWS; }

  bool    supportsSinglePlayer() const override { return true; }
  size_t  aiInput(uint8_t playerId, uint8_t* buf, size_t cap) override;

  // Won and lost together: see NetGame::teamGame().
  bool    teamGame()    const override { return true; }

  // The lobby shows everyone's weapon, so a squad can see it is about to go in
  // with four Bombs and re-pick before anybody presses start. This is the whole
  // reason the roster grew a game-defined byte.
  uint8_t lobbyTag() const override { return _myWeapon; }
  void    drawLobbyTag(Display& d, uint8_t tag, const CRGB& c,
                       int x, int y, int w, int h) const override;
  uint8_t seriesRounds() const override { return 0; }   // rematch until someone backs out

  void   begin(uint8_t arenaW, uint8_t arenaH, uint8_t myId,
               uint8_t numPlayers, uint32_t seed, const CRGB* colors) override;

  size_t serializeInput(uint8_t* buf, size_t cap, const LocalInput& in) override;
  void   applyState(const uint8_t* buf, size_t len) override;

  void   applyInput(uint8_t playerId, const uint8_t* buf, size_t len) override;
  void   hostTick() override;
  size_t serializeState(uint8_t* buf, size_t cap) override;

  bool   isOver(uint8_t& winnerId) const override;
  void   render(Display& disp) const override;

  // This device's chosen loadout, set by the Swarm submenu and persisted there.
  // Read by serializeInput on every sample, so a change takes effect as soon as
  // the sim will accept one.
  void    setLocalWeapon(uint8_t w){ if (w < SWM_W_COUNT) _myWeapon = w; }
  uint8_t localWeapon() const { return _myWeapon; }

  const SwarmSim& sim() const { return _sim; }

private:
  SwarmSim _sim;
  Audio*   _audio = nullptr;
  uint8_t  _myId = 0;
  uint8_t  _myWeapon = SWM_W_BLASTER;
  CRGB     _colors[NET_MAX_PLAYERS];

  // ---- AI wingman (solo only; nothing calls aiInput in a networked match) ---
  // Rolled from the seed in begin(), which SinglePlayer re-rolls for every run
  // and every retry after a loss -- so the wingman turns up with something
  // different each time instead of five Blasters in a row.
  uint8_t  _aiWeapon = SWM_W_BLASTER;
  // Trigger latch for the Shield, the one weapon whose hold cannot be decided
  // from this tick alone. See aiInput().
  bool     _aiHold = false;

  // Client-side audio latches. A client never runs the sim, so it has no event
  // bits to map -- it has to notice what changed between two snapshots instead.
  uint8_t  _lastLives = 0;
  uint8_t  _lastWave  = 0;
  uint8_t  _lastFx    = 0;

  void voiceFromEvents(uint16_t ev);
  void voiceFromState();
  void drawHud(Display& disp) const;
};
