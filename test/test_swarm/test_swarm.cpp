// ============================================================================
//  Native tests for the Swarm simulation (src/games/swarm/swarm_sim.cpp).
//
//  Swarm is a co-op game, and the two things co-op gets wrong are the two
//  things hardest to eyeball on a 16x16 panel: a weapon that cannot actually
//  kill something (which only shows up as "that run felt unwinnable"), and
//  friendly fire that nobody notices because a teammate's death looks like
//  their own mistake. Both are pinned here.
//
//  The load-bearing suite is weaponsKillEverything(): players may all pick the
//  same weapon, so every weapon must be able to kill every alien. That is not
//  a nice property, it is the constraint the whole loadout design rests on.
//
//  Run with:  pio test -e native
// ============================================================================
#include <unity.h>
#include <cstdio>
#include "games/swarm/swarm_sim.h"
#include "games/swarm/waves.h"

// Mirrors of constants private to swarm_sim.cpp. Repeated on purpose: if
// somebody retunes the feel, these fail and say so rather than drifting.
static const int TICK_CHARGE_MAX = 25;
static const int TICK_RESPAWN    = 50;
static const int TICK_BREAK      = 45;
static const int NET_STATE_CAP   = 240;   // NET_STATE_MAX, from net_proto.h

static const uint8_t ARENA_W = 16;
static const uint8_t ARENA_H = 15;        // MATRIX_H - 1, the HUD row reserved

// ---- helpers ----------------------------------------------------------------

// A run with nothing in the sky, so a test can place exactly what it means to
// test. begin() spawns wave 1 immediately, which is right for the game and
// wrong for a unit test.
static void emptySky(SwarmSim& s, uint8_t players = 1, uint32_t seed = 0xC0FFEE){
  s.begin(ARENA_W, ARENA_H, players, seed);
  s.tClearAliens();
}

// Hold the trigger, releasing periodically so a charged weapon discharges.
// A Blaster does not care; a Laser does, and one helper that serves both is
// what lets the invariant suite loop over every weapon uniformly.
static void holdFire(SwarmSim& s, uint8_t pid, uint8_t wpn, int ticks){
  for (int i = 0; i < ticks; i++){
    bool a = (i % (TICK_CHARGE_MAX + 8)) < TICK_CHARGE_MAX + 2;
    s.setInput(pid, 0, 0, a, false, wpn);
    s.step();
  }
}

// Fire until the sky is clear, then STOP. Firing on past the last kill would
// run into the wave break and repopulate the screen, so a test that just
// counted aliens at the end would be reading wave 2 and calling it a failure.
static bool fireUntilClear(SwarmSim& s, uint8_t pid, uint8_t wpn, int maxTicks){
  const int row = s.player(pid).y / SWM_FP;
  for (int i = 0; i < maxTicks; i++){
    if (s.aliensAlive() == 0) return true;
    // Hold station under the target. The formation marches away while a Laser
    // charges, and these suites are about whether a weapon CAN kill a thing,
    // not about whether the test can aim.
    for (int k = 0; k < SWM_MAX_ALIENS; k++)
      if (s.alien(k).used){ s.tPlacePlayer(pid, s.alien(k).x / SWM_FP, row); break; }
    bool a = (i % (TICK_CHARGE_MAX + 8)) < TICK_CHARGE_MAX + 2;
    s.setInput(pid, 0, 0, a, false, wpn);
    s.step();
  }
  return s.aliensAlive() == 0;
}

static int findBoss(const SwarmSim& s){
  for (int i = 0; i < SWM_MAX_ALIENS; i++)
    if (s.alien(i).used && s.alien(i).type == SWM_A_BOSS) return i;
  return -1;
}

static void idleTicks(SwarmSim& s, uint8_t players, int ticks){
  for (int i = 0; i < ticks; i++){
    for (uint8_t p = 0; p < players; p++) s.setInput(p, 0, 0, false, false, SWM_W_BLASTER);
    s.step();
  }
}

// ---- the wave table ---------------------------------------------------------

static void waveTableFits(void){
  for (int i = 0; i < SWM_WAVES; i++){
    const SwmWaveSpec& s = SWM_WAVE[i];
    int total = s.shooters + s.armor + s.grunts + s.divers;
    TEST_ASSERT_TRUE_MESSAGE(total > 0, "a wave with no aliens would end instantly");
    // Room for the wave AND the boss's escorts, which share the same array.
    TEST_ASSERT_TRUE_MESSAGE(total <= SWM_MAX_ALIENS - 2, "wave overflows the alien array");
    TEST_ASSERT_TRUE(s.cols > 0);

    // Every slot has to land on the panel, or aliens silently fail to spawn.
    int rows = (total + s.cols - 1) / s.cols;
    int maxX = SWM_SLOT_X0 + (s.cols - 1) * SWM_SLOT_DX;
    int maxY = SWM_SLOT_Y0 + (rows - 1) * SWM_SLOT_DY;
    TEST_ASSERT_TRUE_MESSAGE(maxX < ARENA_W, "formation is wider than the panel");
    // The formation must start clear of the player band, or wave 1 opens with
    // aliens already on top of the squad.
    TEST_ASSERT_TRUE_MESSAGE(maxY < ARENA_H - SWM_BAND_ROWS,
                             "formation spawns inside the player band");
    TEST_ASSERT_TRUE(s.marchTicks > 0);
  }
}

static void beginStartsWaveOne(void){
  SwarmSim s;
  s.begin(ARENA_W, ARENA_H, 4, 99);
  TEST_ASSERT_EQUAL(1, s.wave());
  TEST_ASSERT_EQUAL(SWM_PH_WAVE, s.phase());
  TEST_ASSERT_EQUAL(SWM_START_LIVES, s.lives());
  TEST_ASSERT_EQUAL(4, s.numPlayers());
  const SwmWaveSpec& w = SWM_WAVE[0];
  TEST_ASSERT_EQUAL(w.shooters + w.armor + w.grunts + w.divers, s.aliensAlive());
  TEST_ASSERT_FALSE(s.over());
}

// The seed has to replay: it is the only thing keeping a client's idea of the
// run and the host's from diverging if prediction is ever added, and it is what
// makes any of these tests repeatable.
static void seedIsDeterministic(void){
  SwarmSim a, b;
  a.begin(ARENA_W, ARENA_H, 2, 0xABCDEF);
  b.begin(ARENA_W, ARENA_H, 2, 0xABCDEF);
  for (int i = 0; i < 200; i++){
    for (uint8_t p = 0; p < 2; p++){
      a.setInput(p, 40, 0, true, false, SWM_W_BLASTER);
      b.setInput(p, 40, 0, true, false, SWM_W_BLASTER);
    }
    a.step(); b.step();
  }
  uint8_t ba[SWM_STATE_MAX], bb[SWM_STATE_MAX];
  size_t na = a.pack(ba, sizeof(ba)), nb = b.pack(bb, sizeof(bb));
  TEST_ASSERT_EQUAL(na, nb);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(ba, bb, na);
}

// ---- players ----------------------------------------------------------------

static void shipStaysInItsBand(void){
  SwarmSim s;
  emptySky(s);
  s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);          // keep the wave from ending
  s.tPlacePlayer(0, 8, 14);

  for (int i = 0; i < 200; i++){                // hard up and left, forever
    s.setInput(0, -100, 100, false, false, SWM_W_BLASTER);
    s.step();
  }
  const SwmPlayer& p = s.player(0);
  TEST_ASSERT_EQUAL(0, p.x / SWM_FP);
  TEST_ASSERT_EQUAL_MESSAGE(ARENA_H - SWM_BAND_ROWS, p.y / SWM_FP,
                            "a ship escaped the player band");

  for (int i = 0; i < 200; i++){
    s.setInput(0, 100, -100, false, false, SWM_W_BLASTER);
    s.step();
  }
  TEST_ASSERT_EQUAL(ARENA_W - 1, s.player(0).x / SWM_FP);
  TEST_ASSERT_EQUAL(ARENA_H - 1, s.player(0).y / SWM_FP);
}

// A unit that walks away leaves its last input standing in the sim, because
// applyInput simply stops being called. The ship has to park itself.
static void staleInputParksTheShip(void){
  SwarmSim s;
  emptySky(s);
  s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);
  s.tPlacePlayer(0, 8, 14);

  s.setInput(0, 100, 0, true, false, SWM_W_BLASTER);   // hard right, firing
  for (int i = 0; i < 5; i++) s.step();
  int movedTo = s.player(0).x / SWM_FP;
  TEST_ASSERT_TRUE_MESSAGE(movedTo > 8, "the ship never moved at all");

  for (int i = 0; i < 100; i++) s.step();              // ...and then silence
  TEST_ASSERT_EQUAL_MESSAGE(ARENA_W - 1, s.player(0).x / SWM_FP,
                            "expected the ship to coast to the wall, then stop");
  TEST_ASSERT_EQUAL_MESSAGE(0, s.bulletsLive(),
                            "a departed player is still firing");
}

// Parking a departed player's ship is not enough on its own: a parked ship is
// still ON the board, so aliens keep flying into it and each hit costs the
// squad a shared life. A unit that walked away would quietly drain the run.
static void aDepartedPlayerStopsCostingLives(void){
  SwarmSim s;
  emptySky(s, 2);
  s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);         // parked up top; keeps the wave alive
  s.tPlacePlayer(0, 2, 14);
  s.tPlacePlayer(1, 12, 12);

  idleTicks(s, 1, SWM_GONE_TICKS + 10);        // only player 0 is talking
  TEST_ASSERT_FALSE_MESSAGE(s.player(1).alive, "a departed ship is still on the board");
  TEST_ASSERT_EQUAL_MESSAGE(SWM_START_LIVES, s.lives(), "leaving cost the squad a life");

  // Drive something straight through the cell that ship was standing in. If it
  // is still solid the diver rams it, takes blast damage and dies -- so the
  // diver surviving its pass is the assertion, with no life bookkeeping needed.
  s.tPlaceAlien(1, SWM_A_DIVER, 12, 11);
  s.tSetDiving(1);
  idleTicks(s, 1, 1);
  TEST_ASSERT_TRUE_MESSAGE(s.alien(1).used,
                           "the diver hit something: the departed ship is still solid");
  TEST_ASSERT_EQUAL_MESSAGE(SWM_START_LIVES, s.lives(),
                            "a departed player's ship is still absorbing hits");
}

// ...and it has to be reversible, or a two-second radio dropout would delete
// somebody from the run for good.
static void aReturningPlayerComesBack(void){
  SwarmSim s;
  emptySky(s, 2);
  s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);
  s.tPlacePlayer(0, 2, 14);
  s.tPlacePlayer(1, 12, 14);

  idleTicks(s, 1, SWM_GONE_TICKS + 10);        // only player 0 is talking
  TEST_ASSERT_FALSE(s.player(1).alive);

  idleTicks(s, 2, 5);                          // player 1 is back on the air
  TEST_ASSERT_TRUE_MESSAGE(s.player(1).alive, "a player who came back never respawned");
  TEST_ASSERT_EQUAL_MESSAGE(SWM_START_LIVES, s.lives(), "coming back cost a life");
}

// ---- weapons ----------------------------------------------------------------

// THE invariant. Duplicate weapon picks are allowed, so a squad of four Bombs
// has to be survivable -- which means no alien may be immune to anything.
static void weaponsKillEverything(void){
  const uint8_t types[4] = { SWM_A_GRUNT, SWM_A_ARMOR, SWM_A_DIVER, SWM_A_SHOOTER };
  for (uint8_t w = 0; w < SWM_W_COUNT; w++){
    for (int t = 0; t < 4; t++){
      SwarmSim s;
      emptySky(s);
      s.tPlacePlayer(0, 8, 14);
      s.tSetWeapon(0, w);
      // The Shield has no reach: its target has to come to the barrier, which
      // is exactly how it is used in play.
      s.tPlaceAlien(0, types[t], 8, w == SWM_W_SHIELD ? 13 : 10);
      s.tSetLives(200);                       // not a test of the life economy

      bool dead = fireUntilClear(s, 0, w, 240);

      char msg[80];
      snprintf(msg, sizeof(msg), "weapon %u cannot kill alien type %u", w, types[t]);
      TEST_ASSERT_TRUE_MESSAGE(dead, msg);
    }
  }
}

static void armourTakesMoreThanAGrunt(void){
  SwarmSim g, a;
  emptySky(g); emptySky(a);
  g.tPlacePlayer(0, 8, 14);  a.tPlacePlayer(0, 8, 14);
  g.tSetWeapon(0, SWM_W_BLASTER); a.tSetWeapon(0, SWM_W_BLASTER);
  g.tPlaceAlien(0, SWM_A_GRUNT, 8, 8);
  a.tPlaceAlien(0, SWM_A_ARMOR, 8, 8);

  int tg = 0, ta = 0;
  while (g.aliensAlive() && tg < 300){ g.setInput(0,0,0,true,false,SWM_W_BLASTER); g.step(); tg++; }
  while (a.aliensAlive() && ta < 300){ a.setInput(0,0,0,true,false,SWM_W_BLASTER); a.step(); ta++; }
  TEST_ASSERT_TRUE_MESSAGE(tg < 300 && ta < 300, "something refused to die");
  TEST_ASSERT_TRUE_MESSAGE(ta > tg, "armour died as fast as a grunt");
}

// A fully charged beam is worth the wait; releasing early is not free damage.
static void laserRewardsAFullCharge(void){
  SwarmSim full, tap;
  emptySky(full); emptySky(tap);
  full.tPlacePlayer(0, 8, 14); tap.tPlacePlayer(0, 8, 14);
  full.tSetWeapon(0, SWM_W_LASER); tap.tSetWeapon(0, SWM_W_LASER);
  full.tPlaceAlien(0, SWM_A_ARMOR, 8, 6);
  tap .tPlaceAlien(0, SWM_A_ARMOR, 8, 6);

  // The formation marches while you charge, so both cases re-aim before
  // releasing -- exactly what a player does, and it keeps this a test of the
  // beam's damage rather than of the march.
  for (int i = 0; i < TICK_CHARGE_MAX; i++){ full.setInput(0,0,0,true,false,SWM_W_LASER); full.step(); }
  full.tPlacePlayer(0, full.alien(0).x / SWM_FP, 14);
  full.setInput(0, 0, 0, false, false, SWM_W_LASER); full.step();
  TEST_ASSERT_EQUAL_MESSAGE(0, full.aliensAlive(), "a full beam should one-shot armour");

  for (int i = 0; i < 3; i++){ tap.setInput(0,0,0,true,false,SWM_W_LASER); tap.step(); }
  tap.tPlacePlayer(0, tap.alien(0).x / SWM_FP, 14);
  tap.setInput(0, 0, 0, false, false, SWM_W_LASER); tap.step();
  TEST_ASSERT_EQUAL_MESSAGE(1, tap.aliensAlive(), "a tapped beam should not one-shot armour");
}

// The beam is a column, so it hits everything stacked in that column and
// nothing beside it. That reach is the Laser's whole identity.
static void beamHitsTheWholeColumnAndNothingElse(void){
  SwarmSim s;
  emptySky(s);
  s.tPlacePlayer(0, 8, 14);
  s.tSetWeapon(0, SWM_W_LASER);
  s.tPlaceAlien(0, SWM_A_GRUNT, 8, 2);
  s.tPlaceAlien(1, SWM_A_GRUNT, 8, 5);
  s.tPlaceAlien(2, SWM_A_GRUNT, 8, 9);
  s.tPlaceAlien(3, SWM_A_GRUNT, 7, 5);          // one column over: must survive
  s.tPlaceAlien(4, SWM_A_GRUNT, 9, 9);

  for (int i = 0; i < TICK_CHARGE_MAX; i++){ s.setInput(0,0,0,true,false,SWM_W_LASER); s.step(); }
  s.tPlacePlayer(0, s.alien(0).x / SWM_FP, 14);      // re-aim; the block marched
  s.setInput(0, 0, 0, false, false, SWM_W_LASER);
  s.step();
  TEST_ASSERT_EQUAL_MESSAGE(2, s.aliensAlive(), "the beam did not take exactly its own column");
}

// The Bomb detonates on the NEAR face of what it hits, so its nine cells are
// centred on the contact cell -- one shot into the underside of a block takes
// that row and the row above it. Six kills for one trigger pull is the whole
// reason to carry it instead of a Blaster, and the bounded footprint is why it
// is not simply better than one.
static void bombClearsItsFootprint(void){
  SwarmSim s;
  emptySky(s);
  s.tPlacePlayer(0, 8, 14);
  s.tSetWeapon(0, SWM_W_BOMB);
  uint8_t slot = 0;
  for (int y = 6; y <= 7; y++)
    for (int x = 7; x <= 9; x++) s.tPlaceAlien(slot++, SWM_A_GRUNT, x, y);
  s.tPlaceAlien(slot, SWM_A_GRUNT, 12, 7);      // well outside the blast

  const int before = s.aliensAlive();
  int fired = 0;
  for (int i = 0; i < 40 && s.aliensAlive() == before; i++){
    s.setInput(0, 0, 0, true, false, SWM_W_BOMB);
    s.step();
    fired++;
  }
  TEST_ASSERT_EQUAL_MESSAGE(1, s.aliensAlive(), "one bomb should clear its 3x3 footprint");
  TEST_ASSERT_TRUE_MESSAGE(fired < 40, "the bomb never detonated");
}

// The Spread trades reach for width. If its bolts ever stopped expiring it
// would simply be a better Blaster.
static void spreadHasLimitedRange(void){
  SwarmSim near_, far_;
  emptySky(near_); emptySky(far_);
  near_.tPlacePlayer(0, 8, 14); far_.tPlacePlayer(0, 8, 14);
  near_.tSetWeapon(0, SWM_W_SPREAD); far_.tSetWeapon(0, SWM_W_SPREAD);
  near_.tPlaceAlien(0, SWM_A_GRUNT, 8, 10);
  far_ .tPlaceAlien(0, SWM_A_GRUNT, 8, 1);

  TEST_ASSERT_TRUE_MESSAGE(fireUntilClear(near_, 0, SWM_W_SPREAD, 120),
                           "the spread cannot reach 4 rows");
  TEST_ASSERT_FALSE_MESSAGE(fireUntilClear(far_, 0, SWM_W_SPREAD, 120),
                            "the spread reached the top of the panel");
}

// Fire one aimed shot at a stationary ship and report whether the squad paid
// for it. Placed rather than waited for: a real Shooter's cooldown outlasts
// several formation marches, so by the time it fired it would be standing in a
// different column and the shot would miss the ship entirely -- a test that
// passes because nothing was ever aimed at anything.
static bool oneShotCostsALife(bool holdShield){
  SwarmSim s;
  emptySky(s, 1);
  s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);          // keeps the wave running
  s.tPlacePlayer(0, 8, 13);
  s.tSetWeapon(0, SWM_W_SHIELD);
  s.tAddAlienBullet(8, 9);                      // straight down the ship's column

  for (int i = 0; i < 20 && s.lives() == SWM_START_LIVES; i++){
    s.setInput(0, 0, 0, holdShield, false, SWM_W_SHIELD);
    s.step();
  }
  return s.lives() < SWM_START_LIVES;
}

static void shieldBlocksIncomingFire(void){
  TEST_ASSERT_TRUE_MESSAGE(oneShotCostsALife(false),
                           "an unshielded ship survived a shot aimed straight at it "
                           "-- the control case is broken, so the real one proves nothing");
  TEST_ASSERT_FALSE_MESSAGE(oneShotCostsALife(true),
                            "incoming fire got through a raised barrier");
}

// ---- the Shield's orb -------------------------------------------------------
// Holding raises the barrier; letting go throws it. The orb bounces off all
// four walls and off whatever it kills, and only its fuse ends it.

static int orbCount(const SwarmSim& s){
  int n = 0;
  for (int i = 0; i < SWM_MAX_BULLETS; i++)
    if (s.bullet(i).used && s.bullet(i).kind == SWM_B_ORB) n++;
  return n;
}

// Hold for `hold` ticks then release, and return how many ticks the orb lived.
static int orbLifetime(int hold){
  SwarmSim s;
  emptySky(s);
  s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);        // keeps the wave from ending
  s.tPlacePlayer(0, 8, 14);
  s.tSetWeapon(0, SWM_W_SHIELD);
  for (int i = 0; i < hold; i++){ s.setInput(0,0,0,true,false,SWM_W_SHIELD); s.step(); }
  s.setInput(0, 0, 0, false, false, SWM_W_SHIELD); s.step();

  int lived = 0;
  for (int i = 0; i < 500 && orbCount(s); i++){
    lived++;
    s.setInput(0, 0, 0, false, false, SWM_W_SHIELD);
    s.step();
  }
  return lived;
}

static void releasingTheShieldThrowsAnOrb(void){
  SwarmSim s;
  emptySky(s);
  s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);
  s.tPlacePlayer(0, 8, 14);
  s.tSetWeapon(0, SWM_W_SHIELD);

  for (int i = 0; i < 20; i++){ s.setInput(0,0,0,true,false,SWM_W_SHIELD); s.step(); }
  TEST_ASSERT_EQUAL_MESSAGE(0, orbCount(s), "an orb left while the trigger was still held");
  TEST_ASSERT_TRUE_MESSAGE(s.player(0).shieldOn, "the barrier stopped going up");

  s.setInput(0, 0, 0, false, false, SWM_W_SHIELD); s.step();
  TEST_ASSERT_EQUAL_MESSAGE(1, orbCount(s), "letting go threw nothing");
}

// A tap is not a throw, or the Shield would spam short orbs for free.
static void aTapThrowsNothing(void){
  SwarmSim s;
  emptySky(s);
  s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);
  s.tPlacePlayer(0, 8, 14);
  s.tSetWeapon(0, SWM_W_SHIELD);

  s.setInput(0, 0, 0, true,  false, SWM_W_SHIELD); s.step();
  s.setInput(0, 0, 0, false, false, SWM_W_SHIELD); s.step();
  TEST_ASSERT_EQUAL_MESSAGE(0, orbCount(s), "a one-tick tap threw an orb");
}

// The whole bargain: hold longer, keep it longer.
static void aLongerHoldThrowsALongerOrb(void){
  const int shortLife = orbLifetime(12);
  const int longLife  = orbLifetime(40);
  TEST_ASSERT_TRUE_MESSAGE(shortLife > 0, "a 12-tick hold threw nothing at all");
  TEST_ASSERT_TRUE_MESSAGE(longLife > shortLife * 2,
                           "holding three times as long did not buy a meaningfully longer orb");
}

// The wind-up outlives the stamina, and NOTHING throws the orb but a release.
// It used to leave on its own when the meter ran dry, which took the one
// decision the weapon is built around out of the player's hands.
static void holdingPastEmptyKeepsWindingUp(void){
  SwarmSim s;
  emptySky(s);
  s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);
  s.tPlacePlayer(0, 8, 14);
  s.tSetWeapon(0, SWM_W_SHIELD);

  // Long past the ~50 ticks of stamina, still holding.
  for (int i = 0; i < 140; i++){ s.setInput(0,0,0,true,false,SWM_W_SHIELD); s.step(); }
  TEST_ASSERT_EQUAL_MESSAGE(0, orbCount(s), "the orb left on its own before the release");
  TEST_ASSERT_FALSE_MESSAGE(s.player(0).shieldOn,
                            "the barrier was still up on an empty meter");

  s.setInput(0, 0, 0, false, false, SWM_W_SHIELD); s.step();
  TEST_ASSERT_EQUAL_MESSAGE(1, orbCount(s), "the release threw nothing after a long hold");
}

// ...but only up to a point, or holding through a whole wave would beat playing.
static void theWindUpIsCapped(void){
  const int atCap   = orbLifetime(50);
  const int wayPast = orbLifetime(200);
  TEST_ASSERT_TRUE_MESSAGE(atCap > 0, "a full hold threw nothing");
  TEST_ASSERT_EQUAL_MESSAGE(atCap, wayPast, "holding four times as long kept buying orb");
}

// It bounces rather than leaving. If a wall bounce is ever dropped the orb
// simply flies off and the weapon quietly stops existing.
static void theOrbStaysInTheArena(void){
  SwarmSim s;
  emptySky(s);
  s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);
  s.tPlacePlayer(0, 1, 14);                   // launched from the very edge
  s.tSetWeapon(0, SWM_W_SHIELD);
  for (int i = 0; i < 45; i++){ s.setInput(0,0,0,true,false,SWM_W_SHIELD); s.step(); }
  s.setInput(0, 0, 0, false, false, SWM_W_SHIELD); s.step();
  TEST_ASSERT_EQUAL(1, orbCount(s));

  int seen = 0;
  for (int i = 0; i < 200 && orbCount(s); i++){
    for (int k = 0; k < SWM_MAX_BULLETS; k++){
      const SwmBullet& b = s.bullet(k);
      if (!b.used || b.kind != SWM_B_ORB) continue;
      seen++;
      const int cx = b.x / SWM_FP, cy = b.y / SWM_FP;
      TEST_ASSERT_TRUE_MESSAGE(cx >= 0 && cx < ARENA_W, "the orb left through a side wall");
      TEST_ASSERT_TRUE_MESSAGE(cy >= 0 && cy < ARENA_H, "the orb left through the top or floor");
    }
    s.setInput(0, 0, 0, false, false, SWM_W_SHIELD);
    s.step();
  }
  TEST_ASSERT_TRUE_MESSAGE(seen > 60, "the orb vanished long before its fuse ran out");
}

// One throw, several kills. A ricochet that expired on its first alien would be
// a bouncing Blaster bolt, not the reason to carry the weapon.
static void oneOrbKillsSeveral(void){
  SwarmSim s;
  emptySky(s);
  s.tPlacePlayer(0, 8, 14);
  s.tSetWeapon(0, SWM_W_SHIELD);
  for (uint8_t i = 0; i < 10; i++) s.tPlaceAlien(i, SWM_A_GRUNT, 3 + i, 6 + (i % 3));

  for (int i = 0; i < 45; i++){ s.setInput(0,0,0,true,false,SWM_W_SHIELD); s.step(); }
  s.setInput(0, 0, 0, false, false, SWM_W_SHIELD); s.step();

  const int before = s.aliensAlive();
  int fewest = before;
  for (int i = 0; i < 200 && orbCount(s); i++){
    s.setInput(0, 0, 0, false, false, SWM_W_SHIELD);
    s.step();
    if (s.aliensAlive() < fewest) fewest = s.aliensAlive();
  }
  TEST_ASSERT_TRUE_MESSAGE(before - fewest >= 3, "one orb managed fewer than three kills");
}

// Dying mid-hold is not a release.
static void dyingMidHoldThrowsNothing(void){
  SwarmSim s;
  emptySky(s);
  s.tPlacePlayer(0, 8, 12);
  s.tSetWeapon(0, SWM_W_SHIELD);
  // Armour, not a diver: the barrier deals 2 and a diver has 1 hp, so a raised
  // shield intercepts every diver aimed at it -- which is the shield working,
  // and useless for killing the holder. Armour's 3 hp survives the barrier and
  // gets through. Started high enough up that the hold clears SWM_ORB_MIN_HOLD
  // before the hit lands, or this would pass without proving anything.
  s.tPlaceAlien(0, SWM_A_ARMOR, 8, 3);
  s.tSetDiving(0);

  int guard = 0;
  while (s.player(0).alive && guard++ < 200){
    s.setInput(0, 0, 0, true, false, SWM_W_SHIELD);
    s.step();
  }
  TEST_ASSERT_TRUE_MESSAGE(guard < 200, "nothing ever reached the ship");
  TEST_ASSERT_TRUE_MESSAGE(guard > 8, "the ship died before the hold was worth an orb");
  TEST_ASSERT_EQUAL_MESSAGE(0, orbCount(s), "a ship threw an orb by dying");
}

// ---- collision --------------------------------------------------------------

// A bolt covers 1.5 cells per tick, so the endpoint alone would let it skip a
// row. This is the test that fails if the midpoint sample is ever removed.
static void boltsDoNotTunnel(void){
  for (int row = 4; row <= 12; row++){
    SwarmSim s;
    emptySky(s);
    s.tPlacePlayer(0, 8, 14);
    s.tSetWeapon(0, SWM_W_BLASTER);
    s.tPlaceAlien(0, SWM_A_GRUNT, 8, row);

    bool dead = fireUntilClear(s, 0, SWM_W_BLASTER, 60);
    char msg[64];
    snprintf(msg, sizeof(msg), "a bolt passed through an alien on row %d", row);
    TEST_ASSERT_TRUE_MESSAGE(dead, msg);
  }
}

// No friendly fire, stated as a property rather than as a code path: a teammate
// parked directly in the line of fire is untouched and costs the team nothing.
static void noFriendlyFire(void){
  for (uint8_t w = 0; w < SWM_W_COUNT; w++){
    SwarmSim s;
    emptySky(s, 2);
    s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);        // keep the wave running
    s.tPlacePlayer(0, 8, 14);
    s.tPlacePlayer(1, 8, 12);                   // directly above player 0
    s.tSetWeapon(0, w);

    for (int i = 0; i < 200; i++){
      // Keep one harmless alien alive the whole time. Letting the sky empty
      // would open a wave break and spawn wave 2 on top of the pair, and a
      // diver killing the teammate would fail this for a reason that has
      // nothing to do with friendly fire.
      if (s.aliensAlive() == 0) s.tPlaceAlien(0, SWM_A_GRUNT, 0, 0);
      bool a = (i % (TICK_CHARGE_MAX + 8)) < TICK_CHARGE_MAX + 2;
      s.setInput(0, 0, 0, a, false, w);
      s.setInput(1, 0, 0, false, false, SWM_W_BLASTER);
      s.step();
    }
    char msg[72];
    snprintf(msg, sizeof(msg), "weapon %u killed a teammate", w);
    TEST_ASSERT_TRUE_MESSAGE(s.player(1).alive, msg);
    TEST_ASSERT_EQUAL_MESSAGE(SWM_START_LIVES, s.lives(), msg);
  }
}

// ---- the shared life economy ------------------------------------------------

static void alienReachingTheFloorCostsALife(void){
  SwarmSim s;
  emptySky(s);
  s.tPlacePlayer(0, 0, 14);
  s.tPlaceAlien(0, SWM_A_DIVER, 15, ARENA_H - 3);
  s.tSetDiving(0);
  for (int i = 0; i < 120 && s.lives() == SWM_START_LIVES; i++)
    idleTicks(s, 1, 1);
  TEST_ASSERT_EQUAL_MESSAGE(SWM_START_LIVES - 1, s.lives(),
                            "an alien reached the floor for free");
}

static void deathCostsTheTeamAndRespawns(void){
  SwarmSim s;
  emptySky(s, 2);
  s.tPlacePlayer(0, 8, 12);
  s.tPlacePlayer(1, 2, 14);
  s.tPlaceAlien(0, SWM_A_DIVER, 8, 8);
  s.tSetDiving(0);

  int guard = 0;
  while (s.player(0).alive && guard++ < 200) idleTicks(s, 2, 1);
  TEST_ASSERT_TRUE_MESSAGE(guard < 200, "the diver never reached the ship");
  TEST_ASSERT_EQUAL_MESSAGE(SWM_START_LIVES - 1, s.lives(),
                            "one death, one shared life -- the pool is the point");
  TEST_ASSERT_TRUE_MESSAGE(s.player(1).alive, "the wrong player died");

  idleTicks(s, 2, TICK_RESPAWN + 2);
  TEST_ASSERT_TRUE_MESSAGE(s.player(0).alive, "the ship never came back");
  TEST_ASSERT_TRUE_MESSAGE(s.player(0).invuln > 0, "it came back without grace");
}

static void runEndsWhenTheLivesRunOut(void){
  SwarmSim s;
  emptySky(s);
  s.tSetLives(1);
  s.tPlacePlayer(0, 8, 12);
  s.tPlaceAlien(0, SWM_A_DIVER, 8, 8);
  s.tSetDiving(0);

  int guard = 0;
  while (!s.over() && guard++ < 300) idleTicks(s, 1, 1);
  TEST_ASSERT_TRUE_MESSAGE(s.over(), "the run never ended");
  TEST_ASSERT_EQUAL(SWM_PH_LOST, s.phase());
  TEST_ASSERT_FALSE(s.won());

  // A finished run is finished: further ticks must not restart anything.
  uint8_t before = s.phase();
  idleTicks(s, 1, 50);
  TEST_ASSERT_EQUAL(before, s.phase());
}

// ---- waves ------------------------------------------------------------------

static void clearingAWaveAdvancesAfterABreak(void){
  SwarmSim s;
  s.begin(ARENA_W, ARENA_H, 1, 7);
  TEST_ASSERT_EQUAL(1, s.wave());
  s.tClearAliens();
  idleTicks(s, 1, 1);
  TEST_ASSERT_EQUAL_MESSAGE(SWM_PH_BREAK, s.phase(), "an empty sky should open a break");

  idleTicks(s, 1, TICK_BREAK + 2);
  TEST_ASSERT_EQUAL_MESSAGE(2, s.wave(), "the break never ended");
  TEST_ASSERT_EQUAL(SWM_PH_WAVE, s.phase());
  TEST_ASSERT_TRUE(s.aliensAlive() > 0);
}

// A swap is requested mid-fight and lands at the break -- never mid-volley.
static void weaponSwapLandsAtTheBreak(void){
  SwarmSim s;
  s.begin(ARENA_W, ARENA_H, 1, 7);
  s.setInput(0, 0, 0, false, false, SWM_W_BLASTER);
  s.step();
  TEST_ASSERT_EQUAL(SWM_W_BLASTER, s.player(0).weapon);

  for (int i = 0; i < 10; i++){                  // ask for the Bomb, mid-wave
    s.setInput(0, 0, 0, false, false, SWM_W_BOMB);
    s.step();
  }
  TEST_ASSERT_EQUAL_MESSAGE(SWM_W_BLASTER, s.player(0).weapon,
                            "a weapon changed hands mid-wave");

  s.tClearAliens();
  for (int i = 0; i < TICK_BREAK + 4; i++){
    s.setInput(0, 0, 0, false, false, SWM_W_BOMB);
    s.step();
  }
  TEST_ASSERT_EQUAL_MESSAGE(SWM_W_BOMB, s.player(0).weapon,
                            "the swap never landed at the break");
}

// The loadout menu's pick has to be in effect for wave 1, not from wave 2.
static void firstInputAdoptsTheChosenWeapon(void){
  SwarmSim s;
  s.begin(ARENA_W, ARENA_H, 1, 7);
  s.setInput(0, 0, 0, false, false, SWM_W_SHIELD);
  s.step();
  TEST_ASSERT_EQUAL_MESSAGE(SWM_W_SHIELD, s.player(0).weapon,
                            "wave 1 was flown with the wrong weapon");
}

// ---- the boss ---------------------------------------------------------------

static void bossFollowsTheLastWave(void){
  SwarmSim s;
  s.begin(ARENA_W, ARENA_H, 1, 11);
  for (int w = 1; w <= SWM_WAVES; w++){
    TEST_ASSERT_EQUAL(w, s.wave());
    s.tClearAliens();
    idleTicks(s, 1, TICK_BREAK + 3);
  }
  TEST_ASSERT_TRUE_MESSAGE(s.bossActive(), "the boss never arrived");
  TEST_ASSERT_EQUAL(SWM_BOSS_HP, s.bossHp());
  TEST_ASSERT_EQUAL(SWM_WAVES + 1, s.wave());
}

static void theWeakPointIsWorthFinding(void){
  SwarmSim s;
  emptySky(s);
  s.tForceBoss();
  s.tSetLives(200);
  s.tPlacePlayer(0, 8, 14);
  s.tSetWeapon(0, SWM_W_BLASTER);

  uint8_t start = s.bossHp();
  int boss = findBoss(s);
  TEST_ASSERT_TRUE(boss >= 0);
  for (int i = 0; i < 60; i++){
    s.tPlacePlayer(0, s.alien(boss).x / SWM_FP + 1, 14);   // under the body
    s.setInput(0, 0, 0, true, false, SWM_W_BLASTER);
    s.step();
  }
  TEST_ASSERT_TRUE_MESSAGE(s.bossHp() < start, "the boss took no damage at all");
}

static void killingTheBossWinsTheRun(void){
  SwarmSim s;
  emptySky(s);
  s.tForceBoss();
  s.tSetLives(200);
  s.tPlacePlayer(0, 8, 14);
  s.tSetWeapon(0, SWM_W_BLASTER);

  // Stay under the body as it tracks across the top. Forty health at one
  // damage a shot is a long fight on purpose -- that is the finale.
  int guard = 0;
  while (!s.over() && guard++ < 6000){
    int boss = findBoss(s);
    if (boss >= 0) s.tPlacePlayer(0, s.alien(boss).x / SWM_FP + 1, 14);
    s.setInput(0, 0, 0, true, false, SWM_W_BLASTER);
    s.step();
  }
  TEST_ASSERT_TRUE_MESSAGE(s.over(), "the boss could not be killed");
  TEST_ASSERT_TRUE_MESSAGE(s.won(), "killing the boss did not win the run");
  TEST_ASSERT_EQUAL(SWM_PH_WON, s.phase());
  TEST_ASSERT_EQUAL(0, s.bossHp());
}

// ---- the wire ---------------------------------------------------------------

static void snapshotFitsOneFrame(void){
  TEST_ASSERT_TRUE_MESSAGE(SWM_STATE_MAX <= NET_STATE_CAP,
                           "the worst-case snapshot does not fit an ESP-NOW frame");
  // Stated exactly, so a change to any entity cap has to come here and be seen.
  TEST_ASSERT_EQUAL_MESSAGE(180, SWM_STATE_MAX, "the snapshot budget moved");
}

// What the client draws has to be what the host meant. Round-tripping through
// pack/unpack and comparing the re-packed bytes is the cheap way to say so.
static void snapshotRoundTrips(void){
  SwarmSim host;
  host.begin(ARENA_W, ARENA_H, 4, 0x5EED);
  for (int i = 0; i < 300; i++){
    for (uint8_t p = 0; p < 4; p++)
      host.setInput(p, (int8_t)(30 * (p % 3 - 1)), 20, (i % 5) < 3, false, (uint8_t)(p % SWM_W_COUNT));
    host.step();
  }

  uint8_t a[SWM_STATE_MAX];
  size_t na = host.pack(a, sizeof(a));
  TEST_ASSERT_TRUE(na >= SWM_STATE_HEADER && na <= SWM_STATE_MAX);

  SwarmSim client;
  client.begin(ARENA_W, ARENA_H, 4, 1);      // a different seed on purpose
  client.unpack(a, na);

  uint8_t b[SWM_STATE_MAX];
  size_t nb = client.pack(b, sizeof(b));
  TEST_ASSERT_EQUAL_MESSAGE(na, nb, "a re-packed snapshot changed length");
  TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, na);

  TEST_ASSERT_EQUAL(host.phase(),  client.phase());
  TEST_ASSERT_EQUAL(host.wave(),   client.wave());
  TEST_ASSERT_EQUAL(host.lives(),  client.lives());
  TEST_ASSERT_EQUAL(host.bossHp(), client.bossHp());
  TEST_ASSERT_EQUAL(host.aliensAlive(), client.aliensAlive());
}

// A full board is the case the budget was sized for, so measure it rather than
// trusting the arithmetic.
static void aBusyBoardStillFits(void){
  SwarmSim s;
  s.begin(ARENA_W, ARENA_H, 4, 0xBEEF);
  size_t worst = 0;
  for (int i = 0; i < 4000; i++){
    for (uint8_t p = 0; p < 4; p++)
      s.setInput(p, (int8_t)(60 - 40 * p), 0, true, false, (uint8_t)(p % SWM_W_COUNT));
    s.step();
    uint8_t buf[SWM_STATE_MAX];
    size_t n = s.pack(buf, sizeof(buf));
    if (n > worst) worst = n;
    if (s.over()) break;
  }
  TEST_ASSERT_TRUE_MESSAGE(worst > SWM_STATE_HEADER, "nothing was ever packed");
  TEST_ASSERT_TRUE_MESSAGE(worst <= NET_STATE_CAP, "a real board overran the frame");
}

// A whole run, unattended, purely to prove it terminates: no wave can stall
// with the sky empty and the phase stuck, which is the failure mode that would
// leave four people staring at a blank panel.
static void anIdleRunTerminates(void){
  SwarmSim s;
  s.begin(ARENA_W, ARENA_H, 4, 0x1234);
  int guard = 0;
  while (!s.over() && guard++ < 20000) idleTicks(s, 4, 1);
  TEST_ASSERT_TRUE_MESSAGE(s.over(), "an unattended run never ended");
  TEST_ASSERT_EQUAL_MESSAGE(SWM_PH_LOST, s.phase(), "nobody fired and the squad won anyway");
}

// ============================================================================
int main(int, char**){
  UNITY_BEGIN();
  RUN_TEST(waveTableFits);
  RUN_TEST(beginStartsWaveOne);
  RUN_TEST(seedIsDeterministic);

  RUN_TEST(shipStaysInItsBand);
  RUN_TEST(staleInputParksTheShip);
  RUN_TEST(aDepartedPlayerStopsCostingLives);
  RUN_TEST(aReturningPlayerComesBack);

  RUN_TEST(weaponsKillEverything);
  RUN_TEST(armourTakesMoreThanAGrunt);
  RUN_TEST(laserRewardsAFullCharge);
  RUN_TEST(beamHitsTheWholeColumnAndNothingElse);
  RUN_TEST(bombClearsItsFootprint);
  RUN_TEST(spreadHasLimitedRange);
  RUN_TEST(shieldBlocksIncomingFire);
  RUN_TEST(releasingTheShieldThrowsAnOrb);
  RUN_TEST(aTapThrowsNothing);
  RUN_TEST(aLongerHoldThrowsALongerOrb);
  RUN_TEST(holdingPastEmptyKeepsWindingUp);
  RUN_TEST(theWindUpIsCapped);
  RUN_TEST(theOrbStaysInTheArena);
  RUN_TEST(oneOrbKillsSeveral);
  RUN_TEST(dyingMidHoldThrowsNothing);

  RUN_TEST(boltsDoNotTunnel);
  RUN_TEST(noFriendlyFire);

  RUN_TEST(alienReachingTheFloorCostsALife);
  RUN_TEST(deathCostsTheTeamAndRespawns);
  RUN_TEST(runEndsWhenTheLivesRunOut);

  RUN_TEST(clearingAWaveAdvancesAfterABreak);
  RUN_TEST(weaponSwapLandsAtTheBreak);
  RUN_TEST(firstInputAdoptsTheChosenWeapon);

  RUN_TEST(bossFollowsTheLastWave);
  RUN_TEST(theWeakPointIsWorthFinding);
  RUN_TEST(killingTheBossWinsTheRun);

  RUN_TEST(snapshotFitsOneFrame);
  RUN_TEST(snapshotRoundTrips);
  RUN_TEST(aBusyBoardStillFits);
  RUN_TEST(anIdleRunTerminates);
  return UNITY_END();
}

void setUp(void)    {}
void tearDown(void) {}
