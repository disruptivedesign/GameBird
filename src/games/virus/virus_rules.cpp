#include "virus_rules.h"
#include "virus_voice.h"
#include "audio/notes.h"
#include "audio/sfx.h"

// ============================================================================
//                        ===  WRITE YOUR VIRUS HERE  ===
//
//  Each of the four functions below IS a virus. The referee calls it once per
//  tick for every cell that virus owns, hands it that cell (`me`) and the
//  match facts (`world`), and you return ONE action. The same function runs on
//  every cell, every tick, so all coordinated behaviour has to emerge from a
//  rule that only ever sees its own little neighbourhood. That is the game.
//
//  Edit any of the four, then race them from the SETUP screen. The ones you
//  leave alone keep playing their default strategy, so it is fine to start by
//  changing only decideA and pitting it against the other three.
//
//  Everything you can call is declared in virus_api.h. What follows is the
//  guided tour of it.
//
// ----------------------------------------------------------------------------
//  THE MATCH
// ----------------------------------------------------------------------------
//  Board      16 wide, 14 tall. It does not wrap -- the rim reads as
//             Occupant::Edge. The bottom two rows of the panel are the HUD.
//  Start      one cell each, at strength Weak, dropped in your own quadrant.
//  Tick       every 180 ms (~5.5 per second).
//  Winner     most cells owned when it ends. Level on cells -> most total
//             strength. Still level -> draw.
//  Early end  last virus alive wins immediately. Or stalemate: if no virus has
//             gained ground AND no virus's centre of mass has shifted a whole
//             tile over 24 ticks, the match is called there and scored as
//             above. A frontier that trades tiles back and forth counts as
//             stalled -- churn is not progress -- but a swarm on the march does
//             not, so a virus that keeps moving keeps playing.
//
// ----------------------------------------------------------------------------
//  ONE TICK, STEP BY STEP
// ----------------------------------------------------------------------------
//  1. SNAPSHOT  The board is frozen. Every rule call this tick reads that same
//               frozen board, so no cell can react to a move made this tick --
//               not an enemy's, not one of your own. Everything is simultaneous.
//  2. DECIDE    Your function runs once per owned cell, in scan order (left to
//               right, top to bottom). If the cell can pay for what you asked
//               for, it pays and the action is queued; if it cannot, nothing
//               happens and it keeps its energy. Idle costs nothing.
//  3. RESOLVE   Every queued action is checked against the snapshot and applied
//               all at once: damage lands, kills happen, fortifies apply,
//               claimed ground changes hands, movers arrive.
//
// ----------------------------------------------------------------------------
//  ENERGY  --  the one real constraint, and it is YOURS
// ----------------------------------------------------------------------------
//  Every cell has its own purse. `me.energy` is what THIS cell has banked, and
//  it is the only energy this cell can ever spend -- no other cell of yours can
//  take it, and none of them can lend you theirs.
//
//    Grow 2     Spore 6     Move 1     Fortify 1     Attack 1     Idle 0
//
//  Your virus earns a fixed income each tick (it depends on the size of the
//  BOARD, not on how well you are doing), and that income is split evenly
//  across every cell you own. So the more ground you hold, the poorer each
//  individual cell is:
//
//    3 cells   ~2 energy each per tick   -- act every tick
//    30 cells  ~0.3 each per tick        -- act about every third tick
//    90 cells  ~0.1 each per tick        -- act about every tenth tick
//
//  Fractions are kept, not thrown away: a cell earning a tenth of an energy a
//  tick really does have 1 energy after ten ticks. A cell banks up to 8 and
//  anything beyond that is wasted, so a cell with nothing to do is losing you
//  income -- that is the pressure to keep expanding.
//
//    me.canAffordGrow()   can this cell pay the 2 for a Grow right now?
//    me.canAffordAttack() ... and so on, one per action.
//
//  It is an exact answer, not a hint. If it is true, the action WILL happen.
//  If it is false and you ask anyway, nothing happens and the cell keeps its
//  energy -- no other cell of yours is affected either way.
//
//  THE REAL DECISION IS WHEN TO SPEND. A cell with 1 energy can Fortify or
//  Attack or Move right now, or hold out two more ticks and Grow, or hold out
//  a dozen and Spore. Spending small every time means never affording anything
//  big. Returning Action::idle() is how a cell saves.
//
// ----------------------------------------------------------------------------
//  ACTIONS  --  what one cell can do
// ----------------------------------------------------------------------------
//  Action::grow(d)    cost 2  Claim the adjacent tile in direction d; it must
//                             be Empty or Neutral. A NEW cell appears there at
//                             Weak, with an empty purse. You keep your cell too.
//                             Growing costs double because area is how you
//                             score -- owning ground is the whole point.
//
//  Action::move(d)    cost 1  Step into the adjacent tile in direction d -- the
//                             same open ground you could have grown into. THIS
//                             cell goes there, keeping its strength and its
//                             banked energy, and the tile behind it goes empty.
//                             You end the tick with exactly as many cells as
//                             you started, which is why it costs half what
//                             growing does: it buys position, not area.
//                             Worth it to carry a toughened-up cell somewhere
//                             it matters, or to walk away from a fight.
//
//                             TWO THINGS THAT CATCH PEOPLE OUT:
//                             - The tile must be open IN THE SNAPSHOT, so you
//                               cannot move into a tile one of your own cells
//                               is vacating this same tick, and two cells can
//                               never swap places. Conga lines do not work.
//                             - If the tile is contested -- or one of your own
//                               cells claimed it first -- you stay exactly where
//                               you were. A cell is never lost to a failed move.
//
//  Action::attack(d)  cost 1  Knock 1 strength off the enemy cell in direction
//                             d. Damage from every attacker this tick stacks,
//                             and is measured against the target's strength AT
//                             SNAPSHOT -- so a cell that fortifies on the tick
//                             it takes lethal damage still dies, and so does a
//                             cell that tried to walk away from it.
//                             A kill leaves Neutral (scorched) ground that
//                             anyone may grow into. Killing opens a tile, it
//                             does not hand it to you.
//
//  Action::fortify()  cost 1  +1 to this cell's own strength, up to Strongest
//                             (4). Applied after incoming damage. Every point
//                             is one more Attack an enemy has to land, and
//                             total strength breaks a tie on cells at the end.
//
//  Action::spore(d)   cost 6  Seed a new cell THREE tiles away in direction d,
//                             leaping clean over whatever lies between --
//                             enemies, your own cells, walls. Only the landing
//                             tile has to be Empty or Neutral. It lands at Weak
//                             and contests claims exactly like a Grow.
//                             At three times the price of a Grow it is a
//                             terrible way to expand and the only way out of a
//                             box. One cell has to save all six on its own, so
//                             it is a plan, not an impulse -- and a small virus
//                             (rich cells) can afford one far sooner than a
//                             sprawling one.
//
//  Action::idle()     cost 0  Do nothing and keep the energy. This is how you
//                             save up, and it is a real move, not a wasted one.
//
//  Any action can tell you its own price with a.cost(), if you would rather do
//  the budgeting yourself.
//
// ----------------------------------------------------------------------------
//  WHAT YOU CAN SEE  --  `me`, one cell
// ----------------------------------------------------------------------------
//  me.strength         your durability here: Strength::Weak(1), Normal(2),
//                      Strong(3), Strongest(4). Ordered, so comparisons work:
//                      `if (me.strength < Strength::Strong) ...`
//  me.energy           this cell's own banked energy, in whole units.
//  me.neighbour(d)     the tile one step away, d being Dir::N / E / S / W.
//
//  A neighbour tells you three things:
//    .occupant   Empty    open ground, claimable and walkable
//                Neutral  scorched ground (a cell died there), also open
//                Self     one of yours
//                Enemy    someone else's
//                Wall     impassable (later levels; never in v1)
//                Edge     off the board -- nothing works in this direction
//                or just ask: q.isEmpty() isNeutral() isSelf() isEnemy()
//                             isWall() isEdge()
//    .strength   Weak..Strongest when Self or Enemy, None otherwise. This is
//                what tells you whether a neighbour is worth attacking.
//    .enemy_id   which rival owns it (1..4, stable for the whole match; 0 when
//                not an enemy). Use it to gang up on one virus, or to avoid
//                picking a fight with a particular one.
//
//  That is the whole of your sight: your own tile, your own purse, your four
//  neighbours, and whether the tile three steps each way is free to spore into.
//  No board, no coordinates, no idea where you are or how you are doing. The
//  rule is also PURE -- it may not remember anything between calls or between
//  ticks (no statics, no globals; they would break the deterministic replay).
//  Every call starts from nothing.
//
// ----------------------------------------------------------------------------
//  WHAT YOU CAN SEE  --  `world`, the match
// ----------------------------------------------------------------------------
//  world.phase         Phase::Early / Middle / End. Driven by whichever is
//                      further along, the clock or how full the board is, so a
//                      fast-filling match reaches End early. Use it to change
//                      plan over a match -- expand early, dig in late.
//
//  That is all of it. Everything that belongs to you alone -- your strength,
//  your energy -- lives on `me`, which is what keeps your rule thinking about
//  one cell at a time.
//
// ----------------------------------------------------------------------------
//  HELPERS  --  all optional; write the loops yourself if you prefer
// ----------------------------------------------------------------------------
//  CAN I PAY? -- money only, says nothing about what is around you. These are
//  the ones to ask when a cell should hold its energy rather than spend it.
//  me.canAffordGrow()      is there 2 in this cell's purse?
//  me.canAffordFortify()   is there 1?
//  me.canAffordAttack()    is there 1?
//  me.canAffordMove()      is there 1?
//  me.canAffordSpore()     is there 6?
//
//  CAN I DO IT? -- money AND terrain, so a false can mean either "cannot pay
//  yet" or "nowhere to do it". Ask the pair above and below to tell them apart.
//  me.canGrow(d)           can pay the 2 and that neighbour is open ground
//  me.canMove(d)           can pay the 1 and that neighbour is open ground --
//                          you walk onto what you could have claimed, for half
//  me.canSpore(d)          can pay the 6 and the tile 3 steps that way is free
//  me.canFortify()         can pay the 1 and you are not already Strongest
//  me.canAttack(d)         can pay the 1 and there is an enemy that way
//
//  WHAT IS AROUND ME? -- terrain only, no money involved.
//  me.findOpen(d)          sets d to the first open direction (grow or move);
//                          false if you are boxed in
//  me.findSporeTarget(d)   sets d to the first sporeable direction; false if
//                          every landing spot is blocked or off the board
//  me.findWeakestEnemy(d)  sets d to the softest adjacent enemy; false if none
//  me.findStrongestEnemy(d) sets d to the toughest adjacent enemy; false if none
//  me.countAdjacent(o)     how many neighbours are Occupant o. For example
//                          countAdjacent(Occupant::Empty) == 0 means this cell
//                          is on no frontier -- a good test for "I am interior,
//                          so fortify or sit this one out"
//
//  Every find* helper scans N, E, S, W and stops at the first hit, so a virus
//  built purely on them drifts north and east. Writing your own loop -- or
//  choosing by neighbour strength, or by enemy_id -- is where the interesting
//  strategies start.
//
//  You return an Action, and that is the whole of it:
//      return Action::grow(d);
//  (A Decision has a second field, `note`; reserved, ignored in v1.)
//
// ----------------------------------------------------------------------------
//  WATCHING A MATCH
// ----------------------------------------------------------------------------
//  Every tick prints a line to Serial (USB, 115200):
//      [virus] t= 42 | A cells=11 str= 19 bank=  6 act=3/11 | B cells= 8 ...
//  cells and str are what you are scored on, `bank` is the total energy sitting
//  unspent across all your cells, and act=3/11 means eleven of your cells asked
//  to do something and three could pay for it.
//
//  A low act ratio is not automatically bad -- cells saving for a Grow show up
//  as unfunded. But a HIGH bank with a low act ratio means your cells are
//  asking for things they cannot afford and hoarding past the cap of 8, which
//  is income thrown away. Either ask for something cheaper or spread out, so
//  the same income is doing more work.
//
//  On the panel, brightness is strength -- a red pulse is a cell that took a
//  hit and lived, a white one is a kill.
//
// ----------------------------------------------------------------------------
//  THE FOUR STARTERS
// ----------------------------------------------------------------------------
//  All four ship IDENTICAL, and deliberately mediocre: grow into the first open
//  tile, otherwise hit the toughest neighbour, otherwise thicken up. Everyone
//  starts from the same line, so a match out of the box is a draw and the only
//  thing that separates the four is what you do to your own.
//
//  It is left unoptimised on purpose -- the obvious weaknesses are the point,
//  and each is a short edit:
//
//    * It never saves. A cell with 1 energy next to open ground spends it on a
//      Fortify or an Attack instead of banking the 2 for the Grow. Ask
//      canAffordGrow() and idle when it is false and the cell expands instead.
//    * It attacks the STRONGEST neighbour, which takes the most hits to kill.
//      findWeakestEnemy(d) is right there and actually finishes cells off.
//    * It ignores `world`. Phase tells you how far along the match is -- there
//      is a case for expanding Early and fighting at the End.
//    * It never uses Move or Spore. Move steps a cell out of a fight for 1,
//      keeping its strength and its savings; Spore leaps a blockade entirely
//      for 6. Nothing here touches either -- see the ACTIONS section above.
//    * Every find* helper scans N,E,S,W and stops at the first hit, so all four
//      viruses drift north and east. Your own loop does better.
// ============================================================================


// ------------------------------- Virus A - Blue -----------------------------
// Basic implementation of virus.
Decision decideA(const Cell& me, const World& world) {
  (void)world;   // this starter ignores the phase -- see THE FOUR STARTERS above

  // Our direction variable
  Dir d = Dir::N;

  // If there is open ground, and we have enough energy to grow into it, do so.
  if (me.findOpen(d) && me.canGrow(d)){
    return Action::grow(d);
  }

  // No where to grow, attack the strongest enemy.
  if (me.findStrongestEnemy(d) && me.canAttack(d)){
    return Action::attack(d);
  }

  // No where to grow, no where to attack, so fortify.
  if (me.canFortify()){
    return Action::fortify();
  }

  return Action::idle();
}


// ------------------------------- Virus B - Orange ---------------------------
Decision decideB(const Cell& me, const World& world) {
  (void)world;

  // Our direction variable
  Dir d = Dir::N;

  // If there is open ground, and we have enough energy to grow into it, do so.
  if (me.findOpen(d) && me.canGrow(d)){
    return Action::grow(d);
  }

  // No where to grow, attack the strongest enemy.
  if (me.findStrongestEnemy(d) && me.canAttack(d)){
    return Action::attack(d);
  }

  // No where to grow, no where to attack, so fortify.
  if (me.canFortify()){
    return Action::fortify();
  }

  return Action::idle();
}


// ------------------------------- Virus C - Cyan -----------------------------
Decision decideC(const Cell& me, const World& world) {
  (void)world;

  // Our direction variable
  Dir d = Dir::N;

  // If there is open ground, and we have enough energy to grow into it, do so.
  if (me.findOpen(d) && me.canGrow(d)){
    return Action::grow(d);
  }

  // No where to grow, attack the strongest enemy.
  if (me.findStrongestEnemy(d) && me.canAttack(d)){
    return Action::attack(d);
  }

  // No where to grow, no where to attack, so fortify.
  if (me.canFortify()){
    return Action::fortify();
  }

  return Action::idle();
}


// ------------------------------- Virus D - Purple ---------------------------
Decision decideD(const Cell& me, const World& world) {
  (void)world;

  // Our direction variable
  Dir d = Dir::N;

  // If there is open ground, and we have enough energy to grow into it, do so.
  if (me.findOpen(d) && me.canGrow(d)){
    return Action::grow(d);
  }

  // No where to grow, attack the strongest enemy.
  if (me.findStrongestEnemy(d) && me.canAttack(d)){
    return Action::attack(d);
  }

  // No where to grow, no where to attack, so fortify.
  if (me.canFortify()){
    return Action::fortify();
  }

  return Action::idle();
}


// --- referee lookup table (do not edit) -------------------------------------
RuleFn VIRUS_RULES[4] = { decideA, decideB, decideC, decideD };


// ============================================================================
//                        ===  GIVE YOUR VIRUS A VOICE  ===
//
//  Optional. Skip the whole section and your virus plays silently.
//
//  You do NOT play sounds from decide() -- your rule is pure and also runs
//  off-hardware in the native tests, where there is no buzzer (the full reason
//  is in virus_voice.h). Instead you declare which sound goes with which event
//  and the referee plays it for you.
//
//  A sound is a list of {frequency in Hz, milliseconds}; note names come from
//  audio/notes.h and a frequency of REST is silence. Write in the C5..C7
//  octaves -- the piezo is barely audible below that -- and keep anything that
//  fires during play under about 150 ms, which is one game tick.
//
//  There is only ONE buzzer, so across all four viruses at most one sound plays
//  per tick, chosen by how big the event was. Grow and Move fire constantly and
//  will drown everything else if you give them something long, which is why the
//  examples below leave them either silent or down at 12-14 ms.
// ============================================================================

// ---- Virus A: silent. Every field nullptr is the same as no voice at all. ---
// (Delete this and copy B's block if you want to hear A.)

// ---- Virus B: a two-note fanfare on a win, and a tick when it takes ground --
static const Step B_GROW[]  = { {NOTE_D7, 14} };
static const Step B_LOST[]  = { {NOTE_A6, 35}, {NOTE_D6, 60} };
static const Step B_WIN[]   = { {NOTE_D6, 90}, {NOTE_A6, 90}, {NOTE_D7, 200} };

static const Sfx B_SFX_GROW = SFX_OF(B_GROW, PRIO_MINOR);
static const Sfx B_SFX_LOST = SFX_OF(B_LOST, PRIO_MAJOR);
static const Sfx B_SFX_WIN  = SFX_OF(B_WIN,  PRIO_MATCH);

static const VirusVoice VOICE_B = {
  /* onGrow     */ &B_SFX_GROW,
  /* onAttack   */ nullptr,
  /* onDamaged  */ nullptr,
  /* onCellLost */ &B_SFX_LOST,
  /* onMoved    */ nullptr,
  /* onSpore    */ nullptr,
  /* signature  */ nullptr,
  /* victory    */ &B_SFX_WIN,
};

// ---- Virus C: the aggressor, so it is the one you hear hitting things -------
// Borrowing the shared catalog (audio/sfx.h) instead of writing notes is fine
// and is the quickest way to get a virus talking.
static const VirusVoice VOICE_C = {
  /* onGrow     */ nullptr,
  /* onAttack   */ &SFX_HIT,
  /* onDamaged  */ nullptr,
  /* onCellLost */ &SFX_DEATH,
  /* onMoved    */ nullptr,
  /* onSpore    */ nullptr,
  /* signature  */ nullptr,
  /* victory    */ &SFX_WIN,
};

// ---- Virus D: the nomad -- you hear it retreat, and hear it leap ------------
// The footstep has to be tiny: this virus walks somewhere almost every tick.
static const Step D_MOVE[] = { {NOTE_C6, 12} };
static const Step D_SIG[]  = { {NOTE_C6, 50}, {NOTE_G6, 50}, {NOTE_E7, 90} };

static const Sfx D_SFX_MOVE = SFX_OF(D_MOVE, PRIO_MINOR);
static const Sfx D_SFX_SIG  = SFX_OF(D_SIG,  PRIO_MATCH);

static const VirusVoice VOICE_D = {
  /* onGrow     */ nullptr,
  /* onAttack   */ nullptr,
  /* onDamaged  */ nullptr,
  /* onCellLost */ nullptr,
  /* onMoved    */ &D_SFX_MOVE,
  /* onSpore    */ &SFX_SPORE,
  /* signature  */ &D_SFX_SIG,
  /* victory    */ &SFX_WIN,
};

// --- referee lookup table (do not edit) -------------------------------------
const VirusVoice* VIRUS_VOICES[4] = { nullptr, &VOICE_B, &VOICE_C, &VOICE_D };
