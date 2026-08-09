#include "swarm_sim.h"
#include "waves.h"
#include <string.h>

// ============================================================================
//  Feel and tuning. Everything here is in ticks or in Q4.4 cells per tick, at
//  the shared 40 ms match cadence (match_config.h) -- Swarm returns 0 from
//  tickMs() precisely so single-player and multiplayer feel identical.
//
//  Unlike Tron, these are NOT derived from the arena. Tron had to scale because
//  its rev-1 numbers were tuned on a board a quarter the size; Swarm was
//  designed at 16x16 from the start, and a shooter's feel is anchored to the
//  player band and the bullet speeds rather than to the diagonal of the panel.
// ============================================================================

#define SWM_STICK_DEAD       12   // stick %; below this the ship holds station
#define SWM_PLAYER_SPEED      8   // Q4.4/tick at full deflection (~1.3 s across)

#define SWM_CHARGE_MAX       25   // ticks of held A for a full laser (~1 s)
#define SWM_STAM_MAX        100   // shield stamina: 2 s up, 4 s to refill
#define SWM_STAM_DRAIN        2
#define SWM_STAM_RECHARGE     1

#define SWM_RESPAWN_T        50   // ticks a dead ship is off the board (~2 s)
#define SWM_INVULN_T         50   // ticks of blinking grace after it returns

#define SWM_BOMB_SPEED        8   // half a cell: the lob you can watch travel

#define SWM_CD_BLASTER        6   // ~4 shots/s
#define SWM_CD_SPREAD        12
#define SWM_CD_BOMB          17

#define SWM_LIFE_BOLT        24   // long enough to leave the arena
#define SWM_LIFE_SPREAD       5   // ~7 cells: the Spread's whole drawback
#define SWM_LIFE_BOMB        60

#define SWM_DMG_BOLT          1
#define SWM_DMG_BLAST         2
#define SWM_DMG_BEAM          3   // fully charged
#define SWM_DMG_BEAM_WEAK     1   // released early
#define SWM_DMG_SHIELD        2   // what flies into the barrier
#define SWM_DMG_ORB           2

// ---- the Shield's orb -------------------------------------------------------
// Holding raises the barrier as it always did; RELEASING throws it, and what
// you held is what you get. The orb's lifetime is the hold, doubled -- so a
// full 2 s of stamina buys about 4 s of orb. This multiplier is the tuning knob
// for the whole weapon: raise it and the Shield takes over a run, drop it and
// the orb is a parting shot rather than a second wave.
//
// The economy is already the price and no second one is charged. Every other
// weapon fires continuously; the Shield spends its stamina and then has no
// barrier AND no attack for the four seconds it takes to refill. The orb is
// what that dead window buys.
#define SWM_ORB_LIFE_MUL      2
#define SWM_ORB_MIN_HOLD      8   // ticks; below this a tap throws nothing
// Ceiling on the wind-up. Set to the same 50 ticks the stamina used to impose,
// so the strongest possible orb is exactly what it was when running dry threw
// one automatically -- this change moved WHEN it leaves, not how big it gets.
// Past this the hold buys nothing and costs plenty, since the barrier is long
// gone by then; that is the "let go now" pressure.
#define SWM_ORB_MAX_HOLD     50
#define SWM_ORB_SPEED        10   // Q4.4/tick per axis -- under a cell, so no sweep needed

#define SWM_DIVE_DY          10   // Q4.4/tick downward once committed
#define SWM_DIVE_DX           6   // ...and sideways, tracking a ship
#define SWM_SHOOT_CD_MIN     60
#define SWM_SHOOT_CD_VAR     40

#define SWM_BREAK_TICKS      45   // pause between waves (~1.8 s)
#define SWM_FX_TTL            3   // ticks an explosion stays on screen
#define SWM_SHIELD_CD        12   // ticks before a barrier hits the same alien again

// The snapshot has to fit one ESP-NOW frame. NET_STATE_MAX is 240 and lives in
// net_proto.h, which this file cannot include (it pulls in Arduino.h and would
// break the native build), so the number is restated and test_swarm asserts the
// two agree. Virus is the precedent: a game that outgrows the frame does not
// fail loudly at runtime, it silently sends nothing.
static_assert(SWM_STATE_MAX <= 240, "Swarm snapshot exceeds the ESP-NOW payload");

// Bullet velocity table, Q4.4 per tick, indexed by SwmBulletDir. These ARE the
// speeds, not directions to be scaled: a straight bolt covers 1.5 cells a tick,
// a Spread diagonal covers about the same ground on a 42-degree line, and
// incoming alien fire deliberately travels at half that so it can be dodged.
static const int8_t DIR_DX[SWM_D_COUNT] = {   0, -16,  16,   0, -16,  16 };
static const int8_t DIR_DY[SWM_D_COUNT] = { -24, -18, -18,  12,  18,  18 };

// The orb travels the four diagonals at its own, slower pace: it is meant to be
// watched and read across several seconds, not to streak past. Both components
// stay under one cell per tick, which is what lets its collision test the cell
// it landed on rather than sweeping the way a 1.5-cell bolt has to.
static const int8_t ORB_DX[SWM_D_COUNT] = { 0, -SWM_ORB_SPEED,  SWM_ORB_SPEED, 0, -SWM_ORB_SPEED, SWM_ORB_SPEED };
static const int8_t ORB_DY[SWM_D_COUNT] = { 0, -SWM_ORB_SPEED, -SWM_ORB_SPEED, 0,  SWM_ORB_SPEED, SWM_ORB_SPEED };

// A bounce is a remap between diagonals -- flip the axis that hit something.
static inline uint8_t orbFlipX(uint8_t d){
  switch (d){
    case SWM_D_UP_L:   return SWM_D_UP_R;
    case SWM_D_UP_R:   return SWM_D_UP_L;
    case SWM_D_DOWN_L: return SWM_D_DOWN_R;
    case SWM_D_DOWN_R: return SWM_D_DOWN_L;
    default:           return d;
  }
}
static inline uint8_t orbFlipY(uint8_t d){
  switch (d){
    case SWM_D_UP_L:   return SWM_D_DOWN_L;
    case SWM_D_DOWN_L: return SWM_D_UP_L;
    case SWM_D_UP_R:   return SWM_D_DOWN_R;
    case SWM_D_DOWN_R: return SWM_D_UP_R;
    default:           return d;
  }
}

// Both fields are bit-packed into the snapshot's third bullet byte, and both
// are exactly full. Adding a sixth kind or a ninth heading needs the packing to
// change first, and this is what says so before it silently truncates.
static_assert(SWM_B_KIND_COUNT <= 4, "bullet kind no longer fits its 2 bits");
static_assert(SWM_D_COUNT      <= 8, "bullet heading no longer fits its 3 bits");

static const uint8_t ALIEN_HP[SWM_A_COUNT] = { 1, 3, 1, 2, 1 };

// Q4.4 -> whole cells. Negative means "off the top of the board", and the
// callers all bounds-check before they use it, so this stays a plain divide.
static inline int cellOf(int16_t q){ return q / SWM_FP; }
static inline int16_t fpOf(int c){ return (int16_t)(c * SWM_FP + SWM_FP / 2); }

// ============================================================================
//  RNG. xorshift32, seeded from the START frame, so a seed replays a whole run
//  exactly -- which is what lets the tests assert on wave composition.
// ============================================================================
uint32_t SwarmSim::rnd(){
  _rng ^= _rng << 13;
  _rng ^= _rng >> 17;
  _rng ^= _rng << 5;
  return _rng;
}
uint8_t SwarmSim::rndMod(uint8_t n){ return n ? (uint8_t)(rnd() % n) : 0; }

// ============================================================================
//  Lifecycle
// ============================================================================
void SwarmSim::begin(uint8_t w, uint8_t h, uint8_t numPlayers, uint32_t seed){
  _w = w > SWM_MAX_W ? SWM_MAX_W : w;
  _h = h > SWM_MAX_H ? SWM_MAX_H : h;
  _numPlayers = numPlayers > SWM_MAX_PLAYERS ? SWM_MAX_PLAYERS : numPlayers;
  if (_numPlayers == 0) _numPlayers = 1;
  _rng = seed ? seed : 1;

  _phase = SWM_PH_WAVE;
  _wave = 0;
  _lives = SWM_START_LIVES;
  _bossHp = 0; _bossWeak = 0; _bossTimer = 0;
  _breakTimer = 0; _tick = 0;
  _marchDir = 1; _marchCd = 0; _dropQueued = false;
  _diveCd = 0; _diveTicks = 0;

  memset(_a,  0, sizeof(_a));
  memset(_b,  0, sizeof(_b));
  memset(_fx, 0, sizeof(_fx));

  for (uint8_t i = 0; i < SWM_MAX_PLAYERS; i++){
    SwmPlayer& p = _p[i];
    memset(&p, 0, sizeof(p));
    // Spread the squad evenly across the floor: 2, 6, 10, 14 on a 16-wide
    // panel with four players, and still centred with one, two or three.
    int col = (_w * (2 * i + 1)) / (2 * _numPlayers);
    p.x = fpOf(col);
    p.y = fpOf(_h - 1);
    p.alive = i < _numPlayers;
    p.weapon = p.wantWeapon = SWM_W_BLASTER;
    p.stale = SWM_INPUT_STALE;    // nothing has been heard from anyone yet
  }

  uint16_t ev = 0;
  spawnWave(1, ev);
}

// ============================================================================
//  Spawning
// ============================================================================
int SwarmSim::addAlien(uint8_t type, int16_t x, int16_t y){
  for (int i = 0; i < SWM_MAX_ALIENS; i++){
    if (_a[i].used) continue;
    SwmAlien& a = _a[i];
    a.used = true; a.type = type; a.x = x; a.y = y;
    a.hp = ALIEN_HP[type < SWM_A_COUNT ? type : 0];
    a.flags = 0;
    a.sCd = 0;
    a.cd = (uint8_t)(SWM_SHOOT_CD_MIN + rndMod(SWM_SHOOT_CD_VAR));
    return i;
  }
  return -1;
}

// Fill the formation slot by slot, in the order shooters -> armour -> grunts ->
// divers. The order IS the layout: slots run left to right and top to bottom,
// so shooters end up in the high rows where their downward fire has room to
// travel, and divers end up on the bottom row nearest the squad.
void SwarmSim::spawnWave(uint8_t n, uint16_t& ev){
  if (n < 1 || n > SWM_WAVES) return;
  const SwmWaveSpec& s = SWM_WAVE[n - 1];

  _wave = n;
  _marchTicks = s.marchTicks;
  _marchCd = s.marchTicks;
  _marchDir = 1;
  _dropQueued = false;
  _diveTicks = s.diveTicks;
  _diveCd = s.diveTicks;

  const uint8_t counts[4] = { s.shooters, s.armor, s.grunts, s.divers };
  const uint8_t types [4] = { SWM_A_SHOOTER, SWM_A_ARMOR, SWM_A_GRUNT, SWM_A_DIVER };

  uint8_t slot = 0;
  for (int k = 0; k < 4; k++){
    for (uint8_t c = 0; c < counts[k]; c++){
      uint8_t col = slot % s.cols, row = slot / s.cols;
      int cx = SWM_SLOT_X0 + col * SWM_SLOT_DX;
      int cy = SWM_SLOT_Y0 + row * SWM_SLOT_DY;
      if (cx < _w && cy < _h) addAlien(types[k], fpOf(cx), fpOf(cy));
      slot++;
    }
  }
  ev |= SWM_EV_WAVE;
}

void SwarmSim::spawnBoss(uint16_t& ev){
  _wave = SWM_WAVES + 1;
  _bossHp = SWM_BOSS_HP;
  _bossWeak = rndMod(SWM_BOSS_CELLS);
  _bossTimer = 0;
  _marchDir = 1;
  _bossMoveCd = SWM_BOSS_MOVE_T;
  int idx = addAlien(SWM_A_BOSS, fpOf((_w - SWM_BOSS_W) / 2), fpOf(0));
  // The boss's hp field carries the WEAK POINT, not its health: eight body
  // cells index the 3-bit field exactly, and its real health is a whole byte in
  // the snapshot header. That is the only reason the body is 4x2 and not 4x3.
  if (idx >= 0) _a[idx].hp = _bossWeak;
  ev |= SWM_EV_BOSS;
}

void SwarmSim::addBullet(uint8_t kind, uint8_t owner, uint8_t dir,
                         int16_t x, int16_t y, uint8_t dmg, uint8_t life){
  for (int i = 0; i < SWM_MAX_BULLETS; i++){
    if (_b[i].used) continue;
    SwmBullet& b = _b[i];
    b.used = true; b.kind = kind; b.owner = owner; b.dir = dir;
    b.x = x; b.y = y; b.dmg = dmg; b.life = life;
    return;
  }
  // Full: the shot is simply lost. Silently dropping it beats stealing the
  // oldest bullet, which would make a teammate's shot vanish mid-flight.
}

void SwarmSim::addFx(uint8_t kind, int cx, int cy){
  if (cx < 0 || cx >= _w || cy < 0 || cy >= _h) return;
  for (int i = 0; i < SWM_MAX_FX; i++){
    if (_fx[i].used) continue;
    _fx[i].used = true;
    _fx[i].kind = kind;
    _fx[i].cell = (uint8_t)(cy * _w + cx);
    _fx[i].ttl  = SWM_FX_TTL;
    return;
  }
}

// ============================================================================
//  Input
// ============================================================================
void SwarmSim::setInput(uint8_t pid, int8_t x, int8_t y, bool a, bool b, uint8_t weapon){
  if (pid >= _numPlayers) return;
  SwmPlayer& p = _p[pid];
  p.inX = x; p.inY = y; p.inA = a; p.inB = b;
  p.stale = 0;

  // Back on the air. Rejoin through the ordinary respawn path so a returning
  // player arrives at their spawn column with the usual grace, rather than
  // blinking into existence wherever they were standing when the link dropped.
  if (p.gone){
    p.gone = false;
    if (!p.alive) p.respawn = 1;
  }
  if (weapon < SWM_W_COUNT){
    p.wantWeapon = weapon;
    // First contact adopts outright; see SwmPlayer::gotInput.
    if (!p.gotInput){
      p.weapon = weapon;
      p.charge = (weapon == SWM_W_SHIELD) ? SWM_STAM_MAX : 0;
    }
  }
  p.gotInput = true;
}

// ============================================================================
//  One tick
// ============================================================================
uint16_t SwarmSim::step(){
  if (over()) return 0;
  uint16_t ev = 0;
  _tick++;

  stepPlayers(ev);
  stepWeapons(ev);
  stepBullets(ev);
  if (_phase == SWM_PH_WAVE){
    stepAliens(ev);
    if (bossActive()) stepBoss(ev);
  }
  stepShields(ev);
  stepFx();
  checkWaveEnd(ev);
  return ev;
}

void SwarmSim::clampPlayer(SwmPlayer& p) const {
  const int16_t xMin = 0, xMax = (int16_t)((_w - 1) * SWM_FP);
  const int16_t yMin = (int16_t)((_h - SWM_BAND_ROWS) * SWM_FP);
  const int16_t yMax = (int16_t)((_h - 1) * SWM_FP);
  if (p.x < xMin) p.x = xMin;
  if (p.x > xMax) p.x = xMax;
  if (p.y < yMin) p.y = yMin;
  if (p.y > yMax) p.y = yMax;
}

void SwarmSim::stepPlayers(uint16_t& ev){
  (void)ev;
  for (uint8_t i = 0; i < _numPlayers; i++){
    SwmPlayer& p = _p[i];

    // Age the input. A unit that walked away leaves values behind that
    // applyInput() is no longer overwriting, so the ship would fly on and keep
    // firing; parking it here is the game's own answer to the open item in
    // docs/architecture.md section 8, and needs no protocol change.
    if (p.stale < 255) p.stale++;
    if (p.stale >= SWM_INPUT_STALE){ p.inX = p.inY = 0; p.inA = p.inB = false; }

    // Gone, not dead: no life is charged for leaving, and no respawn timer is
    // set, so the slot simply sits empty until that unit starts talking again.
    // See SWM_GONE_TICKS -- parking the ship is not enough when the ship is
    // still something aliens can crash into and the lives are shared.
    if (p.stale >= SWM_GONE_TICKS && !p.gone){
      p.gone = true;
      if (p.alive) addFx(SWM_FX_POP, cellOf(p.x), cellOf(p.y));
      p.alive = false;
      p.shieldOn = false;
      p.charge = 0;
      p.meter = 0;
      p.holdTicks = 0;
      p.respawn = 0;
    }

    // B cycles the weapon, but only as a REQUEST -- it lands at the next wave
    // break (see checkWaveEnd). Edge-detected here because the wire carries the
    // held state, not the press.
    if (p.inB && !p.prevB){
      p.wantWeapon = (uint8_t)((p.wantWeapon + 1) % SWM_W_COUNT);
    }
    p.prevB = p.inB;

    if (p.invuln) p.invuln--;

    if (!p.alive){
      if (p.respawn && --p.respawn == 0 && _lives > 0){
        int col = (_w * (2 * i + 1)) / (2 * _numPlayers);
        p.x = fpOf(col);
        p.y = fpOf(_h - 1);
        p.alive = true;
        p.invuln = SWM_INVULN_T;
        // A Shield player comes back with a full meter. Respawning into four
        // seconds of recharge would mean the support role spends its grace
        // window unable to support anybody.
        p.charge = (p.weapon == SWM_W_SHIELD) ? SWM_STAM_MAX : 0;
        p.shieldOn = false;
      }
      continue;
    }

    int dx = (p.inX > SWM_STICK_DEAD || p.inX < -SWM_STICK_DEAD) ? p.inX : 0;
    int dy = (p.inY > SWM_STICK_DEAD || p.inY < -SWM_STICK_DEAD) ? p.inY : 0;
    p.x = (int16_t)(p.x + dx * SWM_PLAYER_SPEED / 100);
    // +y is UP on the stick and rows grow DOWN on the panel. The flip happens
    // once, here, for the same reason controls.h does it once for the menus.
    p.y = (int16_t)(p.y - dy * SWM_PLAYER_SPEED / 100);
    clampPlayer(p);

    if (p.cooldown) p.cooldown--;
  }
}

void SwarmSim::stepWeapons(uint16_t& ev){
  for (uint8_t i = 0; i < _numPlayers; i++){
    SwmPlayer& p = _p[i];
    if (!p.alive){ p.meter = 0; p.shieldOn = false; continue; }

    const int cx = cellOf(p.x);

    switch (p.weapon){
      case SWM_W_BLASTER:
        if (p.inA && p.cooldown == 0){
          addBullet(SWM_B_BOLT, i, SWM_D_UP, p.x, (int16_t)(p.y - SWM_FP),
                    SWM_DMG_BOLT, SWM_LIFE_BOLT);
          p.cooldown = SWM_CD_BLASTER;
          ev |= SWM_EV_FIRE;
        }
        p.meter = 0;
        break;

      case SWM_W_LASER:
        // Charge while held, fire on release. Releasing early is not punished
        // with nothing -- it fires a weak beam -- because a weapon that can
        // waste a full second of holding on a mistimed release is a weapon
        // nobody picks twice.
        if (p.inA){
          if (p.charge < SWM_CHARGE_MAX){
            p.charge++;
            if (p.charge == SWM_CHARGE_MAX) ev |= SWM_EV_CHARGED;
          }
        } else if (p.charge > 0){
          beam(cx, p.charge >= SWM_CHARGE_MAX ? SWM_DMG_BEAM : SWM_DMG_BEAM_WEAK, i, ev);
          p.charge = 0;
          ev |= SWM_EV_FIRE | SWM_EV_BEAM;
        }
        p.meter = (uint8_t)(p.charge * 15 / SWM_CHARGE_MAX);
        break;

      case SWM_W_BOMB:
        if (p.inA && p.cooldown == 0){
          addBullet(SWM_B_BOMB, i, SWM_D_UP, p.x, (int16_t)(p.y - SWM_FP),
                    SWM_DMG_BLAST, SWM_LIFE_BOMB);
          p.cooldown = SWM_CD_BOMB;
          ev |= SWM_EV_FIRE;
        }
        p.meter = 0;
        break;

      case SWM_W_SPREAD:
        if (p.inA && p.cooldown == 0){
          for (uint8_t d = SWM_D_UP; d <= SWM_D_UP_R; d++)
            addBullet(SWM_B_BOLT, i, d, p.x, (int16_t)(p.y - SWM_FP),
                      SWM_DMG_BOLT, SWM_LIFE_SPREAD);
          p.cooldown = SWM_CD_SPREAD;
          ev |= SWM_EV_FIRE;
        }
        p.meter = 0;
        break;

      case SWM_W_SHIELD:
        // Stamina IS the weapon: the barrier is strong enough to be worth
        // standing behind, so the only thing stopping it being permanent is
        // that it runs out twice as fast as it refills.
        //
        // Letting go throws the barrier: an orb that bounces around the arena
        // for as long as the hold earned it. ONLY letting go -- the throw is
        // the player's decision and nothing takes it for them.
        //
        // Which means the hold outlives the stamina. Once the meter is dry the
        // barrier drops and you are exposed, but the wind-up carries on to
        // SWM_ORB_MAX_HOLD. That gap is the whole tension of the weapon: the
        // last second of charge is bought with cover you no longer have.
        //
        // The barrier does NOT strobe back on in that gap. Stamina only
        // recharges once the trigger is released, so "shield up" lasts exactly
        // as long as the meter and then ends, rather than flickering at the
        // one-in-three duty cycle a recharge-while-held would produce.
        if (p.inA){
          if (p.charge >= SWM_STAM_DRAIN){
            p.shieldOn = true;
            p.charge = (uint8_t)(p.charge - SWM_STAM_DRAIN);
          } else {
            p.shieldOn = false;
          }
          if (p.holdTicks < SWM_ORB_MAX_HOLD) p.holdTicks++;
        } else {
          if (p.holdTicks >= SWM_ORB_MIN_HOLD){
            // Aim it into the arena rather than at the nearest wall, so a shot
            // from the edge does not spend its first bounce getting off it.
            const uint8_t dir = (cx < _w / 2) ? SWM_D_UP_R : SWM_D_UP_L;
            uint16_t life = (uint16_t)p.holdTicks * SWM_ORB_LIFE_MUL;
            addBullet(SWM_B_ORB, i, dir, p.x, (int16_t)(p.y - SWM_FP),
                      SWM_DMG_ORB, (uint8_t)(life > 255 ? 255 : life));
            ev |= SWM_EV_FIRE | SWM_EV_ORB;
          }
          p.holdTicks = 0;
          p.shieldOn = false;
          if (p.charge < SWM_STAM_MAX) p.charge += SWM_STAM_RECHARGE;
        }
        // The meter answers whichever question the player is actually asking.
        // Holding, that is "how big is the orb yet" -- the release is theirs to
        // time. Not holding, it is "when can I raise the barrier again". The
        // barrier's own three pixels already say whether it is up, so the meter
        // never has to spend itself saying so.
        p.meter = p.inA ? (uint8_t)(p.holdTicks * 15 / SWM_ORB_MAX_HOLD)
                        : (uint8_t)(p.charge * 15 / SWM_STAM_MAX);
        break;

      default:
        p.meter = 0;
        break;
    }
    if (p.weapon != SWM_W_SHIELD) p.shieldOn = false;
  }
  // Shield stamina starts full on the tick a player adopts the weapon, which is
  // handled where the swap lands rather than here (see checkWaveEnd).
}

bool SwarmSim::shieldSpan(uint8_t pid, int& x0, int& x1, int& row) const {
  if (pid >= _numPlayers) return false;
  const SwmPlayer& p = _p[pid];
  if (!p.alive || !p.shieldOn || p.weapon != SWM_W_SHIELD) return false;
  int cx = cellOf(p.x), cy = cellOf(p.y);
  row = cy - 1;
  if (row < 0) return false;
  x0 = cx - 1; x1 = cx + 1;
  if (x0 < 0) x0 = 0;
  if (x1 > _w - 1) x1 = _w - 1;
  return true;
}

// ============================================================================
//  Bullets
// ============================================================================
void SwarmSim::stepBullets(uint16_t& ev){
  for (int i = 0; i < SWM_MAX_BULLETS; i++){
    SwmBullet& b = _b[i];
    if (!b.used) continue;

    // ---- the orb ----------------------------------------------------------
    // Its own branch, because it is the one projectile that is not trying to
    // leave: it bounces off all four walls and off whatever it kills, and only
    // its fuse ends it. Both velocity components are under a cell per tick, so
    // testing the cell it landed on is enough -- no sweep.
    if (b.kind == SWM_B_ORB){
      if (b.life == 0){
        addFx(SWM_FX_POP, cellOf(b.x), cellOf(b.y));
        b.used = false;
        continue;
      }
      b.life--;

      b.x = (int16_t)(b.x + ORB_DX[b.dir]);
      b.y = (int16_t)(b.y + ORB_DY[b.dir]);

      const int16_t xMax = (int16_t)((_w - 1) * SWM_FP + SWM_FP - 1);
      const int16_t yMax = (int16_t)((_h - 1) * SWM_FP + SWM_FP - 1);
      if      (b.x < 0)    { b.x = 0;    b.dir = orbFlipX(b.dir); }
      else if (b.x > xMax) { b.x = xMax; b.dir = orbFlipX(b.dir); }
      if      (b.y < 0)    { b.y = 0;    b.dir = orbFlipY(b.dir); }
      else if (b.y > yMax) { b.y = yMax; b.dir = orbFlipY(b.dir); }

      const int ocx = cellOf(b.x), ocy = cellOf(b.y);
      const int hitA = alienAtCell(ocx, ocy);
      const bool hitB = bossActive() && bossCoversCell(ocx, ocy);
      if (hitA >= 0 || hitB){
        if (hitA >= 0) damageAlien(hitA, b.dmg, b.owner, ev);
        else           damageBoss(b.dmg, ocx, ocy, b.owner, ev);
        // Ricochet off what it killed, flipping only the VERTICAL component --
        // the brick-breaker convention. Reversing outright would send the orb
        // back down its own path; keeping the horizontal travel makes it weave
        // up and down THROUGH a formation while it crosses, which is the
        // pinball the weapon is for.
        b.dir = orbFlipY(b.dir);
        if (over()) return;
      }
      continue;
    }

    const int16_t ox = b.x, oy = b.y;
    if (b.kind == SWM_B_BOMB) b.y = (int16_t)(b.y - SWM_BOMB_SPEED);
    else {
      b.x = (int16_t)(b.x + DIR_DX[b.dir]);
      b.y = (int16_t)(b.y + DIR_DY[b.dir]);
    }

    const int cx = cellOf(b.x), cy = cellOf(b.y);
    const bool off = b.x < 0 || cx >= _w || b.y < 0 || cy >= _h;

    if (b.kind == SWM_B_BOMB && (off || b.life == 0)){
      // A bomb that ran out of sky detonates rather than fizzling, so a shot
      // fired at nothing still clears the top of the formation.
      blast(cx < 0 ? 0 : (cx >= _w ? _w - 1 : cx), cy < 0 ? 0 : cy, b.dmg, b.owner, ev);
      b.used = false;
      continue;
    }
    if (off || b.life == 0){ b.used = false; continue; }
    b.life--;

    // ---- swept cells, nearest the origin first ----------------------------
    // A bolt covers 1.5 cells a tick, so testing only where it landed lets it
    // pass straight through the row in between -- the same tunnelling
    // BreakoutSim caps its ball speed to avoid. Nothing here moves more than
    // two cells in a tick, so one midpoint sample closes it exactly.
    int sx[2], sy[2], nCells = 0;
    const int mcx = cellOf((int16_t)((ox + b.x) / 2));
    const int mcy = cellOf((int16_t)((oy + b.y) / 2));
    if ((mcx != cx || mcy != cy) && mcx >= 0 && mcx < _w && mcy >= 0 && mcy < _h){
      sx[nCells] = mcx; sy[nCells] = mcy; nCells++;
    }
    sx[nCells] = cx; sy[nCells] = cy; nCells++;

    if (b.kind == SWM_B_ALIEN){
      bool done = false;
      for (int k = 0; k < nCells && !done; k++){
        // Blocked by a barrier before it reaches anybody.
        for (uint8_t pid = 0; pid < _numPlayers && !done; pid++){
          int x0, x1, row;
          if (shieldSpan(pid, x0, x1, row) && sy[k] == row && sx[k] >= x0 && sx[k] <= x1){
            addFx(SWM_FX_POP, sx[k], sy[k]);
            b.used = false;
            done = true;
          }
        }
        for (uint8_t pid = 0; pid < _numPlayers && !done; pid++){
          SwmPlayer& p = _p[pid];
          if (!p.alive || p.invuln) continue;
          if (cellOf(p.x) == sx[k] && cellOf(p.y) == sy[k]){
            killPlayer(pid, ev);
            b.used = false;
            done = true;
          }
        }
      }
      continue;
    }

    // Player fire. No friendly-fire test is needed anywhere in here: player
    // bullets are only ever compared against aliens, so they pass through
    // ships by construction rather than by an exception somebody could delete.
    for (int k = 0; k < nCells; k++){
      int hit = alienAtCell(sx[k], sy[k]);
      if (hit >= 0){
        if (b.kind == SWM_B_BOMB) blast(sx[k], sy[k], b.dmg, b.owner, ev);
        else                      damageAlien(hit, b.dmg, b.owner, ev);
        b.used = false;
        break;
      }
      if (bossActive() && bossCoversCell(sx[k], sy[k])){
        if (b.kind == SWM_B_BOMB) blast(sx[k], sy[k], b.dmg, b.owner, ev);
        else                      damageBoss(b.dmg, sx[k], sy[k], b.owner, ev);
        b.used = false;
        break;
      }
    }
  }
}

// ============================================================================
//  Aliens
// ============================================================================
int SwarmSim::alienAtCell(int cx, int cy) const {
  for (int i = 0; i < SWM_MAX_ALIENS; i++){
    if (!_a[i].used || _a[i].type == SWM_A_BOSS) continue;
    if (cellOf(_a[i].x) == cx && cellOf(_a[i].y) == cy) return i;
  }
  return -1;
}

int SwarmSim::nearestPlayer(int16_t x) const {
  int best = -1; int32_t bestD = 0x7FFFFFFF;
  for (uint8_t i = 0; i < _numPlayers; i++){
    if (!_p[i].alive) continue;
    int32_t d = _p[i].x - x; if (d < 0) d = -d;
    if (d < bestD){ bestD = d; best = i; }
  }
  return best;
}

void SwarmSim::stepAliens(uint16_t& ev){
  // ---- 1. the formation marches as one block ----
  bool marched = false;
  if (_marchCd && --_marchCd == 0){
    _marchCd = _marchTicks;
    marched = true;
  }

  if (marched){
    // Look before stepping: if the block would put anybody outside the arena,
    // it reverses and drops instead. Latched so a block touching the edge with
    // four aliens at once drops exactly one row, not four.
    _dropQueued = false;
    for (int i = 0; i < SWM_MAX_ALIENS && !_dropQueued; i++){
      const SwmAlien& a = _a[i];
      if (!a.used || a.type == SWM_A_BOSS || (a.flags & SWM_AF_DIVING)) continue;
      int nx = cellOf((int16_t)(a.x + _marchDir * SWM_FP));
      if (nx < 0 || nx >= _w) _dropQueued = true;
    }
    if (_dropQueued){
      _marchDir = (int8_t)-_marchDir;
      for (int i = 0; i < SWM_MAX_ALIENS; i++){
        SwmAlien& a = _a[i];
        if (!a.used || a.type == SWM_A_BOSS || (a.flags & SWM_AF_DIVING)) continue;
        a.y = (int16_t)(a.y + SWM_FP);
      }
    } else {
      for (int i = 0; i < SWM_MAX_ALIENS; i++){
        SwmAlien& a = _a[i];
        if (!a.used || a.type == SWM_A_BOSS || (a.flags & SWM_AF_DIVING)) continue;
        a.x = (int16_t)(a.x + _marchDir * SWM_FP);
      }
    }
  }

  // ---- 2. launch a diver ----
  if (_diveTicks && _diveCd && --_diveCd == 0){
    _diveCd = _diveTicks;
    int candidates[SWM_MAX_ALIENS], n = 0;
    for (int i = 0; i < SWM_MAX_ALIENS; i++)
      if (_a[i].used && _a[i].type == SWM_A_DIVER && !(_a[i].flags & SWM_AF_DIVING))
        candidates[n++] = i;
    if (n) _a[candidates[rndMod((uint8_t)n)]].flags |= SWM_AF_DIVING;
  }

  // ---- 3. per-alien behaviour ----
  for (int i = 0; i < SWM_MAX_ALIENS; i++){
    SwmAlien& a = _a[i];
    if (!a.used || a.type == SWM_A_BOSS) continue;
    a.flags = (uint8_t)(a.flags & ~SWM_AF_HIT);   // the render flash lasts one tick
    if (a.sCd) a.sCd--;

    if (a.flags & SWM_AF_DIVING){
      int target = nearestPlayer(a.x);
      if (target >= 0){
        int16_t tx = _p[target].x;
        if      (a.x < tx - SWM_FP / 2) a.x = (int16_t)(a.x + SWM_DIVE_DX);
        else if (a.x > tx + SWM_FP / 2) a.x = (int16_t)(a.x - SWM_DIVE_DX);
      }
      a.y = (int16_t)(a.y + SWM_DIVE_DY);
      if (a.x < 0) a.x = 0;
      if (cellOf(a.x) >= _w) a.x = (int16_t)((_w - 1) * SWM_FP);
    } else if (a.type == SWM_A_SHOOTER){
      if (a.cd && --a.cd == 0){
        a.cd = (uint8_t)(SWM_SHOOT_CD_MIN + rndMod(SWM_SHOOT_CD_VAR));
        addBullet(SWM_B_ALIEN, SWM_OWNER_ALIEN, SWM_D_DOWN,
                  a.x, (int16_t)(a.y + SWM_FP), 1, SWM_LIFE_BOLT);
      }
    }

    // ---- reached the floor: the squad pays for it ----
    if (cellOf(a.y) >= _h - 1){
      addFx(SWM_FX_POP, cellOf(a.x), _h - 1);
      a.used = false;
      ev |= SWM_EV_LEAK;
      loseLife(ev);
      if (over()) return;
      continue;
    }

    // ---- rammed a ship ----
    const int acx = cellOf(a.x), acy = cellOf(a.y);
    for (uint8_t pid = 0; pid < _numPlayers; pid++){
      SwmPlayer& p = _p[pid];
      if (!p.alive || p.invuln) continue;
      if (cellOf(p.x) == acx && cellOf(p.y) == acy){
        killPlayer(pid, ev);
        damageAlien(i, SWM_DMG_BLAST, SWM_OWNER_ALIEN, ev);
        if (over()) return;
        break;
      }
    }
  }
}

// ============================================================================
//  Boss
// ============================================================================
int SwarmSim::bossSlot() const {
  for (int i = 0; i < SWM_MAX_ALIENS; i++)
    if (_a[i].used && _a[i].type == SWM_A_BOSS) return i;
  return -1;
}

int SwarmSim::bossX() const {
  int s = bossSlot();
  return s < 0 ? -1 : cellOf(_a[s].x);
}

bool SwarmSim::bossCoversCell(int cx, int cy) const {
  int s = bossSlot();
  if (s < 0) return false;
  const int bx = cellOf(_a[s].x), by = cellOf(_a[s].y);
  return cx >= bx && cx < bx + SWM_BOSS_W && cy >= by && cy < by + SWM_BOSS_H;
}

void SwarmSim::stepBoss(uint16_t& ev){
  const int slot = bossSlot();
  if (slot < 0) return;
  SwmAlien& boss = _a[slot];
  if (boss.sCd) boss.sCd--;
  boss.flags = (uint8_t)(boss.flags & ~SWM_AF_HIT);

  // Phase by remaining health: it gets faster and fires more as it goes down,
  // so the fight ends on its most dangerous stretch rather than its safest.
  const uint8_t phase = _bossHp <= SWM_BOSS_PH3 ? 3 : (_bossHp <= SWM_BOSS_PH2 ? 2 : 1);
  const uint8_t moveT   = (uint8_t)(SWM_BOSS_MOVE_T   - (phase - 1) * 2);
  const uint8_t volleyT = (uint8_t)(SWM_BOSS_VOLLEY_T - (phase - 1) * 5);

  if (_bossMoveCd && --_bossMoveCd == 0){
    _bossMoveCd = moveT;
    int bx = cellOf(boss.x);
    if (bx + _marchDir < 0 || bx + _marchDir + SWM_BOSS_W > _w) _marchDir = (int8_t)-_marchDir;
    boss.x = (int16_t)(boss.x + _marchDir * SWM_FP);
  }

  _bossTimer++;

  if (_bossTimer % volleyT == 0){
    int bx = cellOf(boss.x), by = cellOf(boss.y);
    for (int k = 0; k < 2; k++){
      int col = bx + rndMod(SWM_BOSS_W);
      addBullet(SWM_B_ALIEN, SWM_OWNER_ALIEN, SWM_D_DOWN,
                fpOf(col), fpOf(by + SWM_BOSS_H), 1, SWM_LIFE_BOLT);
    }
  }
  if (_bossTimer % SWM_BOSS_SPAWN_T == 0 && aliensAlive() < SWM_MAX_ALIENS - 2){
    int bx = cellOf(boss.x), by = cellOf(boss.y);
    int idx = addAlien(SWM_A_GRUNT, fpOf(bx + rndMod(SWM_BOSS_W)), fpOf(by + SWM_BOSS_H));
    // Escorts dive rather than march: there is no formation left to belong to.
    if (idx >= 0) _a[idx].flags |= SWM_AF_DIVING;
  }
  if (_bossTimer % SWM_BOSS_WEAK_T == 0){
    _bossWeak = rndMod(SWM_BOSS_CELLS);
    boss.hp = _bossWeak;
  }

  // The body is solid: flying into it costs a life exactly like a diver does.
  for (uint8_t pid = 0; pid < _numPlayers; pid++){
    SwmPlayer& p = _p[pid];
    if (!p.alive || p.invuln) continue;
    if (bossCoversCell(cellOf(p.x), cellOf(p.y))){
      killPlayer(pid, ev);
      if (over()) return;
    }
  }
}

void SwarmSim::damageBoss(uint8_t dmg, int cx, int cy, uint8_t byPid, uint16_t& ev){
  if (!bossActive()) return;
  int slot = -1;
  for (int i = 0; i < SWM_MAX_ALIENS; i++)
    if (_a[i].used && _a[i].type == SWM_A_BOSS){ slot = i; break; }
  if (slot < 0) return;

  const int bx = cellOf(_a[slot].x), by = cellOf(_a[slot].y);
  const int wx = bx + (_bossWeak % SWM_BOSS_W);
  const int wy = by + (_bossWeak / SWM_BOSS_W);
  uint16_t d = dmg;
  if (cx == wx && cy == wy) d = (uint16_t)(d * SWM_BOSS_WEAK_X);

  if (byPid < _numPlayers) _p[byPid].damage = (uint16_t)(_p[byPid].damage + d);
  _a[slot].flags |= SWM_AF_HIT;
  ev |= SWM_EV_HIT;

  if (d >= _bossHp){
    _bossHp = 0;
    _a[slot].used = false;
    for (int k = 0; k < SWM_BOSS_CELLS; k++)
      addFx(SWM_FX_BLAST, bx + (k % SWM_BOSS_W), by + (k / SWM_BOSS_W));
    _phase = SWM_PH_WON;
    ev |= SWM_EV_KILL | SWM_EV_WON;
  } else {
    _bossHp = (uint8_t)(_bossHp - d);
  }
}

// ============================================================================
//  Shields
// ============================================================================
void SwarmSim::stepShields(uint16_t& ev){
  for (uint8_t pid = 0; pid < _numPlayers; pid++){
    int x0, x1, row;
    if (!shieldSpan(pid, x0, x1, row)) continue;
    for (int x = x0; x <= x1; x++){
      // sCd is what makes the barrier a wall rather than a blender: it may
      // discharge into any one target about twice a second, so a Grunt dies on
      // contact and Armour has to be held against it, but nothing melts.
      int idx = alienAtCell(x, row);
      if (idx >= 0 && _a[idx].sCd == 0){
        _a[idx].sCd = SWM_SHIELD_CD;
        damageAlien(idx, SWM_DMG_SHIELD, pid, ev);
      }
      if (bossActive() && bossCoversCell(x, row)){
        int bs = bossSlot();
        if (bs >= 0 && _a[bs].sCd == 0){
          _a[bs].sCd = SWM_SHIELD_CD;
          damageBoss(SWM_DMG_SHIELD, x, row, pid, ev);
        }
      }
      if (over()) return;
    }
  }
}

// ============================================================================
//  Damage, death, effects
// ============================================================================
void SwarmSim::damageAlien(int idx, uint8_t dmg, uint8_t byPid, uint16_t& ev){
  if (idx < 0 || idx >= SWM_MAX_ALIENS || !_a[idx].used) return;
  SwmAlien& a = _a[idx];
  uint8_t dealt = dmg < a.hp ? dmg : a.hp;
  if (byPid < _numPlayers) _p[byPid].damage = (uint16_t)(_p[byPid].damage + dealt);

  if (dmg >= a.hp){
    addFx(SWM_FX_POP, cellOf(a.x), cellOf(a.y));
    a.used = false;
    ev |= SWM_EV_KILL;
  } else {
    a.hp = (uint8_t)(a.hp - dmg);
    a.flags |= SWM_AF_HIT;
    ev |= SWM_EV_HIT;
  }
}

void SwarmSim::blast(int cx, int cy, uint8_t dmg, uint8_t byPid, uint16_t& ev){
  addFx(SWM_FX_BLAST, cx, cy);
  ev |= SWM_EV_BLAST;
  for (int y = cy - 1; y <= cy + 1; y++)
    for (int x = cx - 1; x <= cx + 1; x++){
      if (x < 0 || x >= _w || y < 0 || y >= _h) continue;
      int idx = alienAtCell(x, y);
      if (idx >= 0) damageAlien(idx, dmg, byPid, ev);
      if (bossActive() && bossCoversCell(x, y)) damageBoss(dmg, x, y, byPid, ev);
      if (over()) return;
    }
}

void SwarmSim::beam(int cx, uint8_t dmg, uint8_t byPid, uint16_t& ev){
  if (cx < 0 || cx >= _w) return;
  addFx(SWM_FX_BEAM, cx, 0);
  for (int y = 0; y < _h; y++){
    int idx = alienAtCell(cx, y);
    if (idx >= 0) damageAlien(idx, dmg, byPid, ev);
    if (bossActive() && bossCoversCell(cx, y)) damageBoss(dmg, cx, y, byPid, ev);
    if (over()) return;
  }
}

void SwarmSim::killPlayer(uint8_t pid, uint16_t& ev){
  if (pid >= _numPlayers) return;
  SwmPlayer& p = _p[pid];
  if (!p.alive) return;
  addFx(SWM_FX_BLAST, cellOf(p.x), cellOf(p.y));
  p.alive = false;
  p.shieldOn = false;
  p.charge = 0;
  p.meter = 0;
  p.holdTicks = 0;          // dying is not a release: no parting orb
  p.respawn = SWM_RESPAWN_T;
  ev |= SWM_EV_PLAYER_HIT;
  loseLife(ev);
}

void SwarmSim::loseLife(uint16_t& ev){
  if (_lives) _lives--;
  if (_lives == 0){
    _phase = SWM_PH_LOST;
    ev |= SWM_EV_LOST;
  }
}

void SwarmSim::stepFx(){
  for (int i = 0; i < SWM_MAX_FX; i++)
    if (_fx[i].used && --_fx[i].ttl == 0) _fx[i].used = false;
}

void SwarmSim::checkWaveEnd(uint16_t& ev){
  if (_phase == SWM_PH_WAVE){
    if (aliensAlive() == 0 && !bossActive()){
      _phase = SWM_PH_BREAK;
      _breakTimer = SWM_BREAK_TICKS;
    }
    return;
  }
  if (_phase != SWM_PH_BREAK) return;
  if (_breakTimer && --_breakTimer) return;

  // The break is where a requested weapon actually lands. Doing it here rather
  // than the moment B is pressed is the whole point: a squad can re-plan
  // between waves, and nobody can swap out of a bad matchup mid-volley.
  for (uint8_t i = 0; i < _numPlayers; i++){
    SwmPlayer& p = _p[i];
    if (p.weapon != p.wantWeapon){
      p.weapon = p.wantWeapon;
      p.charge = (p.weapon == SWM_W_SHIELD) ? SWM_STAM_MAX : 0;
      p.cooldown = 0;
      // A hold that was interrupted by the swap is not owed an orb later.
      p.holdTicks = 0;
    }
  }
  _phase = SWM_PH_WAVE;
  if (_wave >= SWM_WAVES) spawnBoss(ev);
  else                    spawnWave((uint8_t)(_wave + 1), ev);
}

// ============================================================================
//  Queries
// ============================================================================
uint8_t SwarmSim::aliensAlive() const {
  uint8_t n = 0;
  for (int i = 0; i < SWM_MAX_ALIENS; i++) if (_a[i].used) n++;
  return n;
}

uint8_t SwarmSim::bulletsLive() const {
  uint8_t n = 0;
  for (int i = 0; i < SWM_MAX_BULLETS; i++) if (_b[i].used) n++;
  return n;
}

uint8_t SwarmSim::mvp() const {
  uint8_t best = 0; uint16_t bestD = 0;
  for (uint8_t i = 0; i < _numPlayers; i++)
    if (_p[i].damage > bestD){ bestD = _p[i].damage; best = i; }
  return best;
}

// ============================================================================
//  Wire format
//
//  Variable length: only live entities are sent, so a quiet screen is a short
//  frame. SWM_STATE_MAX is the worst case and is static_asserted above.
// ============================================================================
size_t SwarmSim::pack(uint8_t* buf, size_t cap) const {
  if (cap < SWM_STATE_HEADER) return 0;
  size_t o = SWM_STATE_HEADER;

  uint8_t nA = 0, nB = 0, nF = 0;

  for (uint8_t i = 0; i < _numPlayers; i++){
    if (o + 4 > cap) return 0;
    const SwmPlayer& p = _p[i];
    buf[o++] = (uint8_t)(p.x < 0 ? 0 : p.x);
    buf[o++] = (uint8_t)(p.y < 0 ? 0 : p.y);
    buf[o++] = (uint8_t)(((p.weapon & 7) << 5) | (p.alive ? 0x10 : 0) | (p.meter & 0x0F));
    uint8_t iv = p.invuln > 127 ? 127 : p.invuln;
    buf[o++] = (uint8_t)((iv << 1) | (p.shieldOn ? 1 : 0));
  }
  for (int i = 0; i < SWM_MAX_ALIENS; i++){
    if (!_a[i].used) continue;
    if (o + 3 > cap) break;
    const SwmAlien& a = _a[i];
    buf[o++] = (uint8_t)(a.x < 0 ? 0 : a.x);
    buf[o++] = (uint8_t)(a.y < 0 ? 0 : a.y);
    buf[o++] = (uint8_t)(((a.type & 7) << 5) | ((a.hp & 7) << 2) | (a.flags & 3));
    nA++;
  }
  for (int i = 0; i < SWM_MAX_BULLETS; i++){
    if (!_b[i].used) continue;
    if (o + 3 > cap) break;
    const SwmBullet& b = _b[i];
    buf[o++] = (uint8_t)(b.x < 0 ? 0 : b.x);
    buf[o++] = (uint8_t)(b.y < 0 ? 0 : b.y);
    buf[o++] = (uint8_t)(((b.kind & 3) << 6) | ((b.owner & 7) << 3) | (b.dir & 7));
    nB++;
  }
  for (int i = 0; i < SWM_MAX_FX; i++){
    if (!_fx[i].used) continue;
    if (o + 2 > cap) break;
    buf[o++] = _fx[i].cell;
    buf[o++] = _fx[i].kind;
    nF++;
  }

  buf[0] = _phase;
  buf[1] = _wave;
  buf[2] = _lives;
  buf[3] = _numPlayers;
  buf[4] = _bossHp;
  buf[5] = nA;
  buf[6] = nB;
  buf[7] = nF;
  return o;
}

void SwarmSim::unpack(const uint8_t* buf, size_t len){
  if (len < SWM_STATE_HEADER) return;
  _phase      = buf[0];
  _wave       = buf[1];
  _lives      = buf[2];
  _numPlayers = buf[3] > SWM_MAX_PLAYERS ? SWM_MAX_PLAYERS : buf[3];
  _bossHp     = buf[4];
  uint8_t nA  = buf[5] > SWM_MAX_ALIENS  ? SWM_MAX_ALIENS  : buf[5];
  uint8_t nB  = buf[6] > SWM_MAX_BULLETS ? SWM_MAX_BULLETS : buf[6];
  uint8_t nF  = buf[7] > SWM_MAX_FX      ? SWM_MAX_FX      : buf[7];

  size_t o = SWM_STATE_HEADER;
  for (uint8_t i = 0; i < _numPlayers; i++){
    if (o + 4 > len) return;
    SwmPlayer& p = _p[i];
    p.x = (int16_t)buf[o++];
    p.y = (int16_t)buf[o++];
    uint8_t f = buf[o++];
    p.weapon = (uint8_t)((f >> 5) & 7);
    p.alive  = (f & 0x10) != 0;
    p.meter  = (uint8_t)(f & 0x0F);
    uint8_t g = buf[o++];
    p.invuln   = (uint8_t)(g >> 1);
    p.shieldOn = (g & 1) != 0;
  }
  memset(_a,  0, sizeof(_a));
  memset(_b,  0, sizeof(_b));
  memset(_fx, 0, sizeof(_fx));
  for (uint8_t i = 0; i < nA; i++){
    if (o + 3 > len) return;
    SwmAlien& a = _a[i];
    a.used = true;
    a.x = (int16_t)buf[o++];
    a.y = (int16_t)buf[o++];
    uint8_t f = buf[o++];
    a.type  = (uint8_t)((f >> 5) & 7);
    a.hp    = (uint8_t)((f >> 2) & 7);
    a.flags = (uint8_t)(f & 3);
    if (a.type == SWM_A_BOSS) _bossWeak = a.hp;
  }
  for (uint8_t i = 0; i < nB; i++){
    if (o + 3 > len) return;
    SwmBullet& b = _b[i];
    b.used = true;
    b.x = (int16_t)buf[o++];
    b.y = (int16_t)buf[o++];
    uint8_t f = buf[o++];
    b.kind  = (uint8_t)((f >> 6) & 3);
    b.owner = (uint8_t)((f >> 3) & 7);
    b.dir   = (uint8_t)(f & 7);
  }
  for (uint8_t i = 0; i < nF; i++){
    if (o + 2 > len) return;
    _fx[i].used = true;
    _fx[i].cell = buf[o++];
    _fx[i].kind = buf[o++];
  }
}

// ============================================================================
//  Test seams
// ============================================================================
void SwarmSim::tPlacePlayer(uint8_t pid, int cx, int cy){
  if (pid >= SWM_MAX_PLAYERS) return;
  _p[pid].x = fpOf(cx);
  _p[pid].y = fpOf(cy);
  _p[pid].alive = true;
  _p[pid].invuln = 0;
  clampPlayer(_p[pid]);
}

void SwarmSim::tSetWeapon(uint8_t pid, uint8_t w){
  if (pid >= SWM_MAX_PLAYERS || w >= SWM_W_COUNT) return;
  _p[pid].weapon = _p[pid].wantWeapon = w;
  _p[pid].charge = (w == SWM_W_SHIELD) ? SWM_STAM_MAX : 0;
  _p[pid].gotInput = true;
}

void SwarmSim::tPlaceAlien(uint8_t slot, uint8_t type, int cx, int cy){
  if (slot >= SWM_MAX_ALIENS || type >= SWM_A_COUNT) return;
  SwmAlien& a = _a[slot];
  a.used = true; a.type = type;
  a.x = fpOf(cx); a.y = fpOf(cy);
  a.hp = ALIEN_HP[type];
  a.flags = 0;
  a.sCd = 0;
  a.cd = SWM_SHOOT_CD_MIN;
}

void SwarmSim::tSetDiving(uint8_t slot){
  if (slot < SWM_MAX_ALIENS && _a[slot].used) _a[slot].flags |= SWM_AF_DIVING;
}

void SwarmSim::tAddAlienBullet(int cx, int cy){
  addBullet(SWM_B_ALIEN, SWM_OWNER_ALIEN, SWM_D_DOWN, fpOf(cx), fpOf(cy), 1, SWM_LIFE_BOLT);
}

void SwarmSim::tClearAliens(){
  memset(_a, 0, sizeof(_a));
  _bossHp = 0;
}

void SwarmSim::tForceBoss(){
  tClearAliens();
  uint16_t ev = 0;
  _phase = SWM_PH_WAVE;
  spawnBoss(ev);
}
