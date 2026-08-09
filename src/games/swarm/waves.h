#pragma once
#include <stdint.h>

// ============================================================================
//  The run, as data.
//
//  Six waves and then the boss. Tuning the difficulty curve should be editing
//  this table, not editing the sim -- which is also what makes the curve
//  testable: test_swarm walks the table and asserts every wave fits the
//  formation and the alien budget, so a wave that would overflow the array
//  fails on a PC rather than on somebody's desk.
//
//  ---- The constraint duplicate weapons impose -------------------------------
//  Players may all pick the same weapon (see swarm-plan.md section 2), so NO
//  ALIEN MAY BE IMMUNE TO ANYTHING. Every type here dies to every weapon; what
//  changes between them is how many hits it costs and how hard it is to line
//  up. A squad of four Bombs gets a hard run, never an impossible one.
// ============================================================================

#define SWM_WAVES        6

// Formation geometry. Slots sit on every other cell so a 16-wide panel holds 8
// columns with a gap between them -- adjacent lit pixels would read as one
// wide alien rather than two.
#define SWM_SLOT_DX      2
#define SWM_SLOT_DY      2
#define SWM_SLOT_X0      1
#define SWM_SLOT_Y0      0

struct SwmWaveSpec {
  uint8_t  shooters;    // filled first, so they end up in the top rows
  uint8_t  armor;
  uint8_t  grunts;
  uint8_t  divers;      // filled last -> the bottom row, nearest the squad
  uint8_t  cols;        // formation width in slots
  uint8_t  marchTicks;  // ticks per sideways step; lower = faster
  uint16_t diveTicks;   // ticks between dive launches (0 = nobody dives)
};

// At the 40 ms match tick, marchTicks 14 is a step every ~0.56 s.
static const SwmWaveSpec SWM_WAVE[SWM_WAVES] = {
  //  sh  ar  gr  dv  cols  march  dive
  {    0,  0, 12,  0,    6,    14,    0 },   // 1  grunts, and time to learn the stick
  {    0,  0, 10,  3,    6,    12,  150 },   // 2  the first divers
  {    4,  0,  9,  0,    6,    12,    0 },   // 3  shooters: incoming fire from the top
  {    2,  4,  8,  2,    6,    11,  200 },   // 4  armour arrives
  {    0,  2,  6,  6,    7,    10,   90 },   // 5  dive-heavy
  {    4,  5,  6,  5,    7,     9,  100 },   // 6  everything at once
};

// ---- boss ------------------------------------------------------------------
// A 4x2 body, so its eight cells index the 3-bit weak-point field exactly.
#define SWM_BOSS_W        4
#define SWM_BOSS_H        2
#define SWM_BOSS_CELLS   (SWM_BOSS_W * SWM_BOSS_H)
#define SWM_BOSS_HP      40

// Phase thresholds, as fractions of full health. Crossing one speeds the body
// up and shortens the gap between volleys.
#define SWM_BOSS_PH2     (SWM_BOSS_HP * 2 / 3)
#define SWM_BOSS_PH3     (SWM_BOSS_HP / 3)

#define SWM_BOSS_MOVE_T   8    // ticks per sideways step, phase 1
#define SWM_BOSS_VOLLEY_T 30   // ticks between volleys, phase 1
#define SWM_BOSS_SPAWN_T 100   // ticks between escort grunts
#define SWM_BOSS_WEAK_T  125   // ticks before the weak point moves (~5 s)
#define SWM_BOSS_WEAK_X    3   // damage multiplier on the weak cell
