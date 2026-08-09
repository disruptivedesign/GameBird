#include "swarm.h"
#include "waves.h"
#include "core/palette.h"
#include "audio/sfx.h"

// ============================================================================
//  Render palette.
//
//  Hue alone cannot carry this screen: five player colours already exist in
//  palette.h and two of them are the obvious alien colours. BRIGHTNESS is the
//  primary channel instead -- everything bright belongs to the squad, and
//  everything dim is trying to kill you. That rule survives being glanced at
//  from across a desk in a way a hue rule does not.
// ============================================================================
#define SWM_DIM_ALIEN     115    // ~45%
#define SWM_DIM_BULLET    155    // ~60%
#define SWM_DIM_SHIELD    140
#define SWM_DIM_HUD_PAST   40

static const CRGB ALIEN_COLOR[SWM_A_COUNT] = {
  CRGB(210,  30,  20),   // grunt    deep red
  CRGB(220, 220, 235),   // armour   white
  CRGB(175,  45, 225),   // diver    violet
  CRGB(235, 150,   0),   // shooter  amber
  CRGB(255,  55,  40),   // boss     hot red
};

static inline CRGB dim(const CRGB& c, uint8_t v){ CRGB o = c; o.nscale8_video(v); return o; }

// ---- menu icon: the swarm above, one ship below ----
static const uint8_t SWARM_ICON[8][8] = {
  {0,1,0,0,0,0,1,0},
  {0,0,1,1,1,1,0,0},
  {0,1,1,0,0,1,1,0},
  {0,1,1,1,1,1,1,0},
  {0,0,1,0,0,1,0,0},
  {0,0,0,0,0,0,0,0},
  {0,0,0,3,0,0,0,0},
  {0,0,2,2,2,0,0,0},
};
static const CRGB SWARM_ICON_PAL[] = {
  CRGB::Black, CRGB(210, 30, 20), CRGB(0, 200, 255), CRGB(255, 255, 255),
};

Icon Swarm::menuIcon() const { return { SWARM_ICON, SWARM_ICON_PAL }; }

CRGB Swarm::playerColor(uint8_t playerId) const {
  return playerId < NET_MAX_PLAYERS ? _colors[playerId] : CRGB(255, 255, 255);
}

// ============================================================================
//  Lobby tag: each player's weapon, in a box about five wide and two tall.
//
//  Five of these have to fit down one 16x16 panel next to a colour block each,
//  so there is no room for the submenu's 5x5 glyph and none at all for a name.
//  What survives at this size is the SILHOUETTE -- a single dot, a stack, a
//  blob, a fan, a bar -- which is the same thing the submenu is teaching and
//  the same thing the weapon actually paints on the board.
// ============================================================================
static const uint8_t TAG_GLYPH[SWM_W_COUNT][2][5] = {
  { {0,0,1,0,0},                 // BLASTER  a dot
    {0,0,0,0,0} },
  { {0,0,1,0,0},                 // LASER    a stack
    {0,0,1,0,0} },
  { {0,1,1,1,0},                 // BOMB     a blob
    {0,1,1,1,0} },
  { {1,0,1,0,1},                 // SPREAD   a fan
    {0,1,1,1,0} },
  { {0,0,1,0,0},                 // SHIELD   a bar, and the orb it throws
    {1,1,1,1,1} },
};

void Swarm::drawLobbyTag(Display& d, uint8_t tag, const CRGB& c,
                         int x, int y, int w, int h) const {
  if (tag >= SWM_W_COUNT) return;
  for (int gy = 0; gy < 2 && gy < h; gy++)
    for (int gx = 0; gx < 5 && gx < w; gx++)
      if (TAG_GLYPH[tag][gy][gx]) d.setPixel(x + gx, y + gy, c);
}

// ============================================================================
//  Lifecycle
// ============================================================================
void Swarm::begin(uint8_t arenaW, uint8_t arenaH, uint8_t myId,
                  uint8_t numPlayers, uint32_t seed, const CRGB* colors){
  _myId = myId;
  for (int i = 0; i < NET_MAX_PLAYERS; i++)
    _colors[i] = colors ? colors[i] : PRESET_COLORS[i % NUM_PRESET_COLORS];

  _sim.begin(arenaW, arenaH, numPlayers, seed);
  _lastLives = _sim.lives();
  _lastWave  = _sim.wave();
  _lastFx    = 0;

  // The wingman's loadout for this run. Mixed rather than taken from the low
  // bits directly, because the sim seeds its own wave rolls from the same
  // number and a wingman whose weapon tracked the wave layout would be a
  // pattern somebody eventually notices.
  uint32_t h = seed * 2654435761u;
  h ^= h >> 15;
  _aiWeapon = (uint8_t)(h % SWM_W_COUNT);
  _aiHold   = false;
}

// ============================================================================
//  Input -- three bytes: two axes and a byte of buttons plus the weapon.
// ============================================================================
size_t Swarm::serializeInput(uint8_t* buf, size_t cap, const LocalInput& in){
  if (cap < 3) return 0;
  // Stick space, unflipped: +y is UP here and the sim does the row flip once,
  // exactly where Tron does it. A game reading this is deciding a direction,
  // not addressing a row.
  buf[0] = (uint8_t)(int8_t)(in.x < -100 ? -100 : (in.x > 100 ? 100 : in.x));
  buf[1] = (uint8_t)(int8_t)(in.y < -100 ? -100 : (in.y > 100 ? 100 : in.y));
  buf[2] = (uint8_t)((in.a ? 0x01 : 0) | (in.b ? 0x02 : 0) | ((_myWeapon & 7) << 2));
  return 3;
}

void Swarm::applyInput(uint8_t pid, const uint8_t* buf, size_t len){
  if (len < 3) return;
  _sim.setInput(pid, (int8_t)buf[0], (int8_t)buf[1],
                (buf[2] & 0x01) != 0, (buf[2] & 0x02) != 0,
                (uint8_t)((buf[2] >> 2) & 7));
}

// ============================================================================
//  AI wingman. Produces the same three bytes a human would, so it drives
//  through the identical applyInput() path -- the reason SinglePlayer needs no
//  special case for a co-op game.
//
//  Its weapon is rolled per run (see begin()) rather than fixed, so solo play
//  is not the same fight every time. That used to be a Blaster always, for a
//  good reason: the bot held the trigger down forever, and two of the five
//  weapons do their work on the RELEASE. A Laser would have charged to full and
//  sat there; a Shield would have drained its stamina and then stood in the
//  open, because stamina only refills once the trigger is let go. Randomising
//  the weapon without teaching the trigger to let go would have handed the
//  wingman a dead stick two runs in five.
//
//  So the hold is now the weapon's to decide. The three that fire on a cooldown
//  still hold it down; the two that fire on release let go at the moment that
//  is worth the most.
// ============================================================================
size_t Swarm::aiInput(uint8_t pid, uint8_t* buf, size_t cap){
  if (cap < 3) return 0;
  buf[0] = 0;
  buf[1] = (uint8_t)(int8_t)-100;     // hold the floor
  buf[2] = (uint8_t)(0x01 | (_aiWeapon << 2));

  if (pid >= _sim.numPlayers()) return 3;
  const SwmPlayer& me = _sim.player(pid);
  if (!me.alive) return 3;

  // `meter` rather than the raw charge: the sim's tuning constants are private
  // to swarm_sim.cpp, and meter is the same number normalised to 0..15 for the
  // screen. Full is full on either scale.
  bool hold = true;
  switch (_aiWeapon){
    case SWM_W_LASER:
      // Let go exactly at full, which fires the strong beam rather than the
      // weak one, then charge again from zero.
      hold = me.meter < 15;
      break;

    case SWM_W_SHIELD:
      // The only weapon whose answer needs yesterday. Held, `meter` reports the
      // orb's wind-up; released, it reports stamina -- so a single tick cannot
      // tell "barrier up" from "barrier dry", and reading it either way gives a
      // barrier that strobes at a one-in-three duty cycle, which is the exact
      // thing the sim's own comment says it was shaped to avoid.
      //
      // Latch instead: hold until the barrier actually drops, then let go --
      // which throws the orb the hold has been earning -- and stay off until
      // stamina reads full again.
      if      (_aiHold && !me.shieldOn) _aiHold = false;
      else if (!_aiHold && me.meter >= 15) _aiHold = true;
      hold = _aiHold;
      break;

    default:                            // Blaster, Bomb, Spread: cooldown-fired
      break;
  }
  if (!hold) buf[2] &= (uint8_t)~0x01;

  // Chase the nearest threat: the alien with the greatest y, since that is the
  // one about to cost the team a life.
  int16_t targetX = me.x;
  int     lowest  = -1;
  for (int i = 0; i < SWM_MAX_ALIENS; i++){
    const SwmAlien& a = _sim.alien(i);
    if (!a.used || a.type == SWM_A_BOSS) continue;
    if (a.y > lowest){ lowest = a.y; targetX = a.x; }
  }
  if (lowest < 0 && _sim.bossActive()){
    for (int i = 0; i < SWM_MAX_ALIENS; i++){
      const SwmAlien& a = _sim.alien(i);
      if (a.used && a.type == SWM_A_BOSS){ targetX = (int16_t)(a.x + SWM_BOSS_W * SWM_FP / 2); break; }
    }
  }

  if      (targetX > me.x + SWM_FP / 2) buf[0] = (uint8_t)(int8_t) 100;
  else if (targetX < me.x - SWM_FP / 2) buf[0] = (uint8_t)(int8_t)-100;
  return 3;
}

// ============================================================================
//  Host tick and state
// ============================================================================
void Swarm::hostTick(){
  uint16_t ev = _sim.step();
  voiceFromEvents(ev);
  _lastLives = _sim.lives();
  _lastWave  = _sim.wave();
}

size_t Swarm::serializeState(uint8_t* buf, size_t cap){
  return _sim.pack(buf, cap);
}

void Swarm::applyState(const uint8_t* buf, size_t len){
  _sim.unpack(buf, len);
  voiceFromState();
}

bool Swarm::isOver(uint8_t& winnerId) const {
  if (!_sim.over()) return false;
  // Somebody has to be named for the result screen's border and framed bar, so
  // a cleared run reports its MVP by damage dealt. teamGame() is what stops the
  // runner reading that as "everyone else lost".
  winnerId = _sim.won() ? _sim.mvp() : NET_PID_NONE;
  return true;
}

// ============================================================================
//  Voice.
//
//  One piezo, one voice, and four players firing four times a second: a sound
//  per shot would be a solid tone that stomped everything worth hearing. So
//  nothing routine makes a noise. Only the things a player would look up for.
//
//  The host maps the sim's own event bits. A client never runs the sim, so it
//  has no events -- it diffs consecutive snapshots instead. Neither role plays
//  the win/lose sting: the runner already does that at the end of a match, and
//  doubling it just makes the first one lose to the second.
// ============================================================================
void Swarm::voiceFromEvents(uint16_t ev){
  if (!_audio) return;
  if      (ev & SWM_EV_PLAYER_HIT) _audio->play(SFX_DEATH);
  else if (ev & SWM_EV_LEAK)       _audio->play(SFX_DEATH);
  else if (ev & SWM_EV_BOSS)       _audio->play(SFX_SPORE);
  else if (ev & SWM_EV_WAVE)       _audio->play(SFX_GROW);
  else if (ev & SWM_EV_ORB)        _audio->play(SFX_SPORE);
  else if (ev & SWM_EV_CHARGED)    _audio->play(SFX_SPORE);
  else if (ev & SWM_EV_KILL)       _audio->play(SFX_HIT);
}

void Swarm::voiceFromState(){
  uint8_t fx = 0;
  for (int i = 0; i < SWM_MAX_FX; i++) if (_sim.fx(i).used) fx++;

  if (_audio){
    if      (_sim.lives() < _lastLives) _audio->play(SFX_DEATH);
    else if (_sim.wave()  != _lastWave) _audio->play(SFX_GROW);
    else if (fx > _lastFx)              _audio->play(SFX_HIT);
  }
  _lastLives = _sim.lives();
  _lastWave  = _sim.wave();
  _lastFx    = fx;
}

// ============================================================================
//  Render -- identical on every unit, since all of them draw the same snapshot.
// ============================================================================
void Swarm::render(Display& disp) const {
  disp.clear();

  const uint8_t W = _sim.w();

  // ---- aliens ----
  for (int i = 0; i < SWM_MAX_ALIENS; i++){
    const SwmAlien& a = _sim.alien(i);
    if (!a.used) continue;
    const int ax = a.x / SWM_FP, ay = a.y / SWM_FP;

    if (a.type == SWM_A_BOSS){
      // The weak point rides in the hp field (see SwarmSim::spawnBoss) and is
      // drawn bright, because finding it is the fight.
      for (int k = 0; k < SWM_BOSS_CELLS; k++){
        const int bx = ax + (k % SWM_BOSS_W), by = ay + (k / SWM_BOSS_W);
        const bool weak = (k == a.hp);
        disp.setPixel(bx, by, weak ? CRGB(255, 230, 120)
                                   : dim(ALIEN_COLOR[SWM_A_BOSS], SWM_DIM_ALIEN));
      }
      continue;
    }

    const uint8_t t = a.type < SWM_A_COUNT ? a.type : 0;
    disp.setPixel(ax, ay, (a.flags & SWM_AF_HIT) ? CRGB(255, 255, 255)
                                                 : dim(ALIEN_COLOR[t], SWM_DIM_ALIEN));
  }

  // ---- bullets ----
  for (int i = 0; i < SWM_MAX_BULLETS; i++){
    const SwmBullet& b = _sim.bullet(i);
    if (!b.used) continue;
    const int bx = b.x / SWM_FP, by = b.y / SWM_FP;
    CRGB c;
    if (b.kind == SWM_B_ALIEN)      c = dim(CRGB(255, 90, 0), SWM_DIM_BULLET);
    else if (b.owner < NET_MAX_PLAYERS) c = dim(_colors[b.owner], SWM_DIM_BULLET);
    else                            c = dim(CRGB(255, 255, 255), SWM_DIM_BULLET);
    // A bomb is drawn brighter than a bolt: it is slow, and a dim slow pixel
    // reads as a stray alien rather than as ordnance in flight.
    if (b.kind == SWM_B_BOMB) c = dim(c, 220);
    // The orb is not ordnance in flight, it is a thing living on the board for
    // several seconds, bouncing among the aliens. At bullet dimness it would
    // read as one of them; at full brightness it reads as unmistakably yours,
    // which is the rule the whole screen is built on.
    if (b.kind == SWM_B_ORB) c = (b.owner < NET_MAX_PLAYERS) ? _colors[b.owner] : CRGB(255, 255, 255);
    disp.setPixel(bx, by, c);
  }

  // ---- shields, drawn under the ships so an overlap favours the ship ----
  for (uint8_t pid = 0; pid < _sim.numPlayers(); pid++){
    int x0, x1, row;
    if (!_sim.shieldSpan(pid, x0, x1, row)) continue;
    for (int x = x0; x <= x1; x++) disp.setPixel(x, row, dim(_colors[pid], SWM_DIM_SHIELD));
  }

  // ---- ships ----
  for (uint8_t pid = 0; pid < _sim.numPlayers(); pid++){
    const SwmPlayer& p = _sim.player(pid);
    if (!p.alive) continue;
    // Grace blinks. A ship that came back invulnerable and looked identical to
    // one that did not is how a player learns the window exists by dying at the
    // end of it.
    if (p.invuln && ((p.invuln / 3) & 1)) continue;
    disp.setPixel(p.x / SWM_FP, p.y / SWM_FP, _colors[pid]);
  }

  // ---- effects, last: they are the loudest thing on screen and should sit
  //      on top of whatever they just destroyed ----
  for (int i = 0; i < SWM_MAX_FX; i++){
    const SwmFx& f = _sim.fx(i);
    if (!f.used) continue;
    const int fx = f.cell % W, fy = f.cell / W;
    switch (f.kind){
      case SWM_FX_BEAM:
        for (int y = 0; y < _sim.h(); y++) disp.setPixel(fx, y, CRGB(180, 240, 255));
        break;
      case SWM_FX_BLAST:
        for (int y = fy - 1; y <= fy + 1; y++)
          for (int x = fx - 1; x <= fx + 1; x++) disp.setPixel(x, y, CRGB(255, 190, 60));
        break;
      default:
        disp.setPixel(fx, fy, CRGB(255, 255, 255));
        break;
    }
  }

  drawHud(disp);

  // The run's verdict frames the panel: the MVP's colour on a win, red on a
  // wipe. Every unit shows the same thing, because every unit has the phase.
  if (_sim.over())
    disp.border(_sim.won() ? _colors[_sim.mvp()] : CRGB(140, 0, 0));
}

// ---- HUD: the bottom panel row, which is not part of the arena --------------
// Left: the shared life pool, the number whose falling is everybody's problem.
// Right: how far the run has come -- or, once the boss is up, how much of it is
// left, which is the only number that matters at that point.
void Swarm::drawHud(Display& disp) const {
  const int row = MATRIX_H - 1;

  for (int i = 0; i < SWM_START_LIVES; i++)
    disp.setPixel(i, row, i < _sim.lives() ? CRGB(0, 200, 60) : CRGB(24, 6, 0));

  if (_sim.bossActive()){
    const int x0 = 5, x1 = MATRIX_W - 1, span = x1 - x0 + 1;
    const int lit = (_sim.bossHp() * span + SWM_BOSS_HP - 1) / SWM_BOSS_HP;
    for (int i = 0; i < span; i++)
      disp.setPixel(x0 + i, row, i < lit ? CRGB(200, 40, 30) : CRGB(20, 4, 4));
    return;
  }

  const int pips = SWM_WAVES + 1;
  const int x0   = MATRIX_W - pips;
  for (int i = 0; i < pips; i++){
    const int waveNo = i + 1;
    CRGB c;
    if      (waveNo <  _sim.wave()) c = dim(CRGB(0, 140, 200), SWM_DIM_HUD_PAST);
    else if (waveNo == _sim.wave()) c = CRGB(0, 180, 255);
    else                            c = CRGB(6, 10, 14);
    disp.setPixel(x0 + i, row, c);
  }
}
