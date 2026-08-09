#pragma once
#include <stdint.h>
#include <stddef.h>

// ============================================================================
//  The Swarm simulation: every rule, no hardware.
//
//  DELIBERATELY FREE OF <Arduino.h>, FastLED and Display, exactly like
//  breakout_sim.h and tetris_sim.h -- this is compiled by the native test env
//  (see platformio.ini) so waves, weapons, the boss and the shared-life
//  economy can be pinned without flashing a board. If this header ever pulls
//  in the Arduino core, `pio test -e native` stops building. Keep it
//  stdint-only.
//
//  ---- Who runs what --------------------------------------------------------
//  Swarm is a streamed NetGame, so only the HOST ever calls step(). Clients
//  call unpack() on the host's snapshot and render it. That split is what lets
//  most of the state in here stay host-only: cooldowns, bullet lifetimes, the
//  RNG, wave timers and per-player input never cross the wire, because nobody
//  on the other end simulates anything. Only what has to be DRAWN is packed.
//
//  ---- Why positions are sub-cell -------------------------------------------
//  Same reason as Breakout's ball. A thing that moves whole cells per tick has
//  exactly one speed, and the whole design here is that a Blaster bolt, a
//  lobbed Bomb and a marching Grunt move at visibly different rates. Positions
//  are Q4.4 (SWM_FP = one cell), which is also exactly one byte per axis on
//  the wire for a panel up to 16 wide -- so the sub-cell precision is free.
//
//  ---- Co-op, and what that costs -------------------------------------------
//  There is one shared life pool and no friendly fire. Both are load-bearing:
//  the shared pool is what makes another player's death your problem, and
//  friendly fire on a 16-wide band with four ships would make the Spread and
//  the Bomb unusable next to a teammate.
// ============================================================================

#define SWM_MAX_W          16
#define SWM_MAX_H          16
#define SWM_MAX_PLAYERS     4    // the runner allows 5; this game wants 4
#define SWM_MAX_ALIENS     28    // 24-strong formation + boss spawns
#define SWM_MAX_BULLETS    20
#define SWM_MAX_FX          6

#define SWM_FP             16    // one cell, in fixed point (Q4.4)

#define SWM_START_LIVES     3    // shared across the whole squad
#define SWM_BAND_ROWS       4    // player airspace, measured up from the floor

// A player whose input has not arrived for this long is parked: zero stick,
// no trigger. Net::getInput already stops handing the host a stale blob after
// 500 ms, but that only stops applyInput() being CALLED -- the values it wrote
// last time just stand, so a departed player would otherwise leave a ship
// flying and firing forever. See docs/architecture.md section 8.
#define SWM_INPUT_STALE    30    // ticks (~1.2 s at the 40 ms match tick)

// Longer silence still, and the ship comes OFF the board. Parking it is not
// enough on its own: a parked ship is still a target, and because the lives are
// shared, every alien that flies into an abandoned ship costs the players who
// are still here. A unit that walked away would quietly drain the run.
//
// Deliberately well clear of SWM_INPUT_STALE, so the two mean different things:
// a brief radio dropout parks you and you fly again, three seconds of silence
// takes you out. Leaving is free -- no life is charged -- and it reverses the
// moment input starts arriving again.
#define SWM_GONE_TICKS     75    // ticks (~3 s)

// ---- weapons ---------------------------------------------------------------
// The order is the wire order and the pick order in the loadout menu. Adding
// one means widening the 3-bit field in the packed player byte.
enum SwmWeapon : uint8_t {
  SWM_W_BLASTER = 0,   // 1px bolt, fast, reliable
  SWM_W_LASER,         // charge, release, the whole column
  SWM_W_BOMB,          // slow lob, 3x3 blast on contact
  SWM_W_SPREAD,        // 3-bolt fan, short range
  SWM_W_SHIELD,        // 3-wide barrier; the support role
  SWM_W_COUNT
};

// ---- aliens ----------------------------------------------------------------
enum SwmAlienType : uint8_t {
  SWM_A_GRUNT = 0,     // 1 hp, marches
  SWM_A_ARMOR,         // 3 hp, marches
  SWM_A_DIVER,         // 1 hp, peels off and accelerates at a ship
  SWM_A_SHOOTER,       // 2 hp, holds high and fires down
  SWM_A_BOSS,          // 4x2 body, hp in the snapshot header
  SWM_A_COUNT
};

// Alien flag bits (2 bits on the wire, so there is room for exactly these two).
#define SWM_AF_DIVING   0x01     // has left the formation
#define SWM_AF_HIT      0x02     // took damage last tick -- render flashes white

// ---- bullets ---------------------------------------------------------------
enum SwmBulletKind : uint8_t {
  SWM_B_BOLT = 0,      // Blaster and Spread
  SWM_B_BOMB,          // lobbed, detonates
  SWM_B_ALIEN,         // incoming
  SWM_B_ORB,           // the Shield's parting gift: bounces, persists, ricochets
  SWM_B_KIND_COUNT
};

// Bullet headings, indexing DIR_DX/DIR_DY in the .cpp. Three bits on the wire.
// The four diagonals are the orb's; a bounce is a remap between them.
enum SwmBulletDir : uint8_t {
  SWM_D_UP = 0,
  SWM_D_UP_L,
  SWM_D_UP_R,
  SWM_D_DOWN,
  SWM_D_DOWN_L,
  SWM_D_DOWN_R,
  SWM_D_COUNT
};

#define SWM_OWNER_ALIEN  7       // the "not a player" owner code (3 bits)

// ---- transient effects -----------------------------------------------------
// Explosions and beams are drawn from the snapshot rather than inferred by the
// client, because they last two ticks and a client that missed the frame in
// which an alien vanished has nothing to infer from.
enum SwmFxKind : uint8_t {
  SWM_FX_POP = 0,      // one cell: something died here
  SWM_FX_BLAST,        // 3x3, centred
  SWM_FX_BEAM,         // the whole column at this cell's x
};

// ---- run phase -------------------------------------------------------------
enum SwmPhase : uint8_t {
  SWM_PH_WAVE = 0,     // fighting
  SWM_PH_BREAK,        // between waves; weapon switches land here
  SWM_PH_WON,          // boss dead
  SWM_PH_LOST,         // shared lives exhausted
};

// What happened during one step(). The sim neither draws nor makes noise: it
// reports, and swarm.cpp maps these onto the sfx.h catalog. This is what keeps
// the sim linkable in the native test env.
enum SwmEvent : uint16_t {
  SWM_EV_FIRE       = 1u << 0,   // somebody's weapon went off
  SWM_EV_HIT        = 1u << 1,   // damage landed on an alien
  SWM_EV_KILL       = 1u << 2,   // an alien died
  SWM_EV_BLAST      = 1u << 3,   // a bomb detonated
  SWM_EV_BEAM       = 1u << 4,   // a laser fired
  SWM_EV_CHARGED    = 1u << 5,   // a laser reached full charge
  SWM_EV_PLAYER_HIT = 1u << 6,   // a ship died; the team lost a life
  SWM_EV_LEAK       = 1u << 7,   // an alien reached the floor; the team lost a life
  SWM_EV_WAVE       = 1u << 8,   // a new wave just began
  SWM_EV_BOSS       = 1u << 9,   // the boss just arrived
  SWM_EV_WON        = 1u << 10,
  SWM_EV_LOST       = 1u << 11,
  SWM_EV_ORB        = 1u << 12,  // a shield let go of an energy orb
};

// ---- packed snapshot sizing ------------------------------------------------
// The wire format is [8 B header | 4 B/player | 3 B/alien | 3 B/bullet |
// 2 B/fx], variable length -- only live entities are sent. This is the worst
// case, and swarm_sim.cpp static_asserts it against NET_STATE_MAX's 240 so the
// budget cannot drift unnoticed. Virus is the game that proved this matters.
#define SWM_STATE_HEADER   8
#define SWM_STATE_MAX     (SWM_STATE_HEADER + SWM_MAX_PLAYERS * 4 + \
                           SWM_MAX_ALIENS * 3 + SWM_MAX_BULLETS * 3 + \
                           SWM_MAX_FX * 2)

struct SwmPlayer {
  int16_t  x, y;          // Q4.4, arena space
  uint8_t  weapon;        // in effect now
  uint8_t  wantWeapon;    // adopted at the next wave break
  bool     alive;
  uint8_t  charge;        // HOST-ONLY raw: laser ticks held, or shield stamina
  // The same two things normalised to 0..15 -- a laser's charge and a shield's
  // remaining stamina are one meter as far as the screen is concerned. It is a
  // separate field from `charge` because the two are on different scales, and a
  // client rendering the raw number would draw a different bar than the host.
  uint8_t  meter;
  bool     shieldOn;
  uint8_t  cooldown;      // ticks until the trigger works again
  // HOST-ONLY: how long the Shield's trigger has been held. It is the orb's
  // fuse -- released, it becomes the orb's lifetime -- so it is the one number
  // that makes a long hold worth more than a short one.
  uint8_t  holdTicks;
  uint8_t  respawn;       // ticks until this ship comes back (0 = not dead)
  uint8_t  invuln;        // ticks of post-respawn grace; nonzero = blinking
  uint16_t damage;        // running tally, for the MVP report

  // ---- host-only input mirror ----
  int8_t   inX, inY;
  bool     inA, inB, prevB;
  uint8_t  stale;         // ticks since applyInput last touched this slot
  // The FIRST input from a device adopts its weapon outright; every later
  // change waits for a wave break. Without this a player who picked Laser in
  // the loadout menu would fly wave 1 with a Blaster, because the run starts
  // before any input has arrived and the swap only lands at a break.
  bool     gotInput;
  // Off the board entirely, as opposed to dead: no respawn timer, no life
  // charged, and nothing drawn. Cleared by the next input that arrives.
  bool     gone;
};

struct SwmAlien {
  int16_t  x, y;          // Q4.4, arena space
  uint8_t  type;
  uint8_t  hp;            // boss keeps its hp in the header instead
  uint8_t  flags;
  uint8_t  cd;            // host-only: fire timer
  // host-only: ticks before a barrier may discharge into this alien again. A
  // shield sits on a cell for as long as it is held, so without this it would
  // deal its damage every tick -- fifty a second, which melts the boss.
  uint8_t  sCd;
  bool     used;
};

struct SwmBullet {
  int16_t  x, y;          // Q4.4
  uint8_t  kind;
  uint8_t  owner;         // playerId, or SWM_OWNER_ALIEN
  uint8_t  dir;
  uint8_t  dmg;           // host-only
  uint8_t  life;          // host-only: ticks left; what gives Spread its range
  bool     used;
};

struct SwmFx {
  uint8_t  cell;          // y * w + x
  uint8_t  kind;
  uint8_t  ttl;           // host-only
  bool     used;
};

class SwarmSim {
public:
  // ---- lifecycle ----
  // numPlayers is clamped to SWM_MAX_PLAYERS. The seed drives wave composition
  // and every random choice the boss makes, so the same seed replays exactly --
  // which is what lets the tests assert on a whole run.
  void begin(uint8_t w, uint8_t h, uint8_t numPlayers, uint32_t seed);

  // ---- input (host) ----
  // Raw stick percentages and button holds, plus the weapon that device says it
  // is carrying. The weapon is a REQUEST: it is adopted at the next wave break,
  // never mid-fight, so nobody swaps out of a bad matchup halfway through a
  // boss volley.
  void setInput(uint8_t pid, int8_t x, int8_t y, bool a, bool b, uint8_t weapon);

  // ---- one tick (host) ----
  uint16_t step();                     // -> SwmEvent bits

  // ---- run state ----
  uint8_t  phase()      const { return _phase; }
  bool     over()       const { return _phase == SWM_PH_WON || _phase == SWM_PH_LOST; }
  bool     won()        const { return _phase == SWM_PH_WON; }
  uint8_t  wave()       const { return _wave; }      // 1..SWM_WAVES, then boss
  uint8_t  lives()      const { return _lives; }
  uint8_t  bossHp()     const { return _bossHp; }
  bool     bossActive() const { return _bossHp > 0; }
  uint8_t  numPlayers() const { return _numPlayers; }
  uint8_t  w()          const { return _w; }
  uint8_t  h()          const { return _h; }

  // Who dealt the most damage. Reported as the "winner" so the result screen
  // has somebody to frame, but nothing punitive hangs off it -- see
  // NetGame::teamGame() and docs/plans/swarm-plan.md section 5.
  uint8_t  mvp() const;

  // ---- entities, for rendering and for the tests ----
  const SwmPlayer& player(uint8_t i) const { return _p[i < SWM_MAX_PLAYERS ? i : 0]; }
  const SwmAlien&  alien (uint8_t i) const { return _a[i < SWM_MAX_ALIENS  ? i : 0]; }
  const SwmBullet& bullet(uint8_t i) const { return _b[i < SWM_MAX_BULLETS ? i : 0]; }
  const SwmFx&     fx    (uint8_t i) const { return _fx[i < SWM_MAX_FX     ? i : 0]; }
  uint8_t aliensAlive() const;
  uint8_t bulletsLive() const;

  // Where the Shield's barrier sits for player i: three cells one row ahead.
  // Returns false if that player is not currently holding one up. Rendering and
  // the collision path both ask, so the geometry is defined once.
  bool shieldSpan(uint8_t pid, int& x0, int& x1, int& row) const;

  // ---- wire format ----
  size_t pack(uint8_t* buf, size_t cap) const;    // host -> snapshot
  void   unpack(const uint8_t* buf, size_t len);  // client -> adopt

  // ---- test seams ----
  // Placing a thing exactly beats playing until it happens to be there, for the
  // same reason BreakoutSim::placeBall exists.
  void tPlacePlayer(uint8_t pid, int cx, int cy);
  void tSetWeapon(uint8_t pid, uint8_t w);
  void tPlaceAlien(uint8_t slot, uint8_t type, int cx, int cy);
  void tClearAliens();
  void tSetDiving(uint8_t slot);   // commit an alien to its dive right now
  // Incoming fire, placed exactly. Waiting for a real Shooter to produce a
  // shot means waiting out its cooldown while the formation marches it several
  // cells sideways -- so a test that wanted a bullet on a particular column
  // would quietly stop getting one and pass because nothing was ever aimed.
  void tAddAlienBullet(int cx, int cy);
  void tSetLives(uint8_t n) { _lives = n; }
  void tForceBoss();

private:
  uint8_t  _w = 0, _h = 0;
  uint8_t  _numPlayers = 0;
  uint8_t  _phase = SWM_PH_WAVE;
  uint8_t  _wave = 0;              // 0 before the first spawn
  uint8_t  _lives = 0;
  uint8_t  _bossHp = 0;
  uint8_t  _bossWeak = 0;          // index into the 4x2 body
  uint16_t _bossTimer = 0;
  // The boss paces itself. It shares the arena with escort grunts that still
  // run the formation code, so it cannot share the formation's counter.
  uint8_t  _bossMoveCd = 0;
  uint16_t _breakTimer = 0;
  uint16_t _tick = 0;
  uint32_t _rng = 1;

  // Formation march: direction, the accumulator that paces it, and a latch so
  // the block drops exactly once per edge contact however many aliens touched.
  int8_t   _marchDir = 1;
  uint8_t  _marchCd = 0;
  uint8_t  _marchTicks = 12;
  bool     _dropQueued = false;
  uint16_t _diveCd = 0;
  uint16_t _diveTicks = 0;

  SwmPlayer _p[SWM_MAX_PLAYERS] = {};
  SwmAlien  _a[SWM_MAX_ALIENS]  = {};
  SwmBullet _b[SWM_MAX_BULLETS] = {};
  SwmFx     _fx[SWM_MAX_FX]     = {};

  uint32_t rnd();
  uint8_t  rndMod(uint8_t n);

  void  spawnWave(uint8_t n, uint16_t& ev);
  void  spawnBoss(uint16_t& ev);
  int   addAlien(uint8_t type, int16_t x, int16_t y);
  void  addBullet(uint8_t kind, uint8_t owner, uint8_t dir,
                  int16_t x, int16_t y, uint8_t dmg, uint8_t life);
  void  addFx(uint8_t kind, int cx, int cy);

  void  stepPlayers(uint16_t& ev);
  void  stepWeapons(uint16_t& ev);
  void  stepBullets(uint16_t& ev);
  void  stepAliens(uint16_t& ev);
  void  stepBoss(uint16_t& ev);
  void  stepShields(uint16_t& ev);
  void  stepFx();
  void  checkWaveEnd(uint16_t& ev);

  void  damageAlien(int idx, uint8_t dmg, uint8_t byPid, uint16_t& ev);
  void  damageBoss(uint8_t dmg, int cx, int cy, uint8_t byPid, uint16_t& ev);
  void  killPlayer(uint8_t pid, uint16_t& ev);
  void  loseLife(uint16_t& ev);
  void  blast(int cx, int cy, uint8_t dmg, uint8_t byPid, uint16_t& ev);
  void  beam(int cx, uint8_t dmg, uint8_t byPid, uint16_t& ev);

  int   alienAtCell(int cx, int cy) const;
  int   nearestPlayer(int16_t x) const;
  int   bossSlot() const;                      // -1 when there is no boss
  bool  bossCoversCell(int cx, int cy) const;
  int   bossX() const;
  void  clampPlayer(SwmPlayer& p) const;
};
