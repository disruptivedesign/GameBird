#pragma once
#include <stdint.h>
#include <initializer_list>

// ============================================================================
//  Virus rule API -- the ONLY header a virus author needs to read.
//
//  You write a `decide()` function (see virus_rules.cpp). Every cycle the
//  referee calls it once for each cell you own. It is handed that cell and its
//  four orthogonal neighbours (N,E,S,W) and returns one action.
//
//  The rule is PURE: it may not remember anything between calls or between
//  ticks, and it sees almost nothing -- its own tile, its own energy, its 4
//  neighbours, and (only for aiming a Spore) whether the tile 3 steps away each
//  way is free. All coordinated behaviour has to emerge from a rule that only
//  ever sees its own neighbourhood -- that is the game. World.button is the one
//  exception: a live signal from outside the board, the same for every cell of
//  every player this tick, not something the rule remembers on its own.
// ============================================================================

// What sits in a neighbouring tile.
enum class Occupant : uint8_t {
  Empty,     // open ground -- claimable with Grow, walkable with Move
  Neutral,   // scorched ground (a cell died here) -- also claimable/walkable
  Self,      // one of your own cells
  Enemy,     // another player's cell
  Wall,      // impassable (later levels only)
  Edge,      // off the board -- nothing can be done toward it
};

// Directions, in the order neighbours are indexed.
enum class Dir : uint8_t { N = 0, E = 1, S = 2, W = 3 };

enum class ActionKind : uint8_t { Idle, Grow, Fortify, Attack, Move, Spore };

// Enumerations for costs
enum class Cost : uint8_t {
  Idle = 0,
  Grow = 2,
  Fortify = 1,
  Attack = 1,
  Move = 1,
  Spore = 6,
};

// One thing a cell can try to do this tick. Build these with the factories
// below, e.g. Action::grow(Dir::N).
struct Action {
  ActionKind kind;
  Dir        dir;   // used by Grow / Attack / Move / Spore; ignored otherwise

  static constexpr Action idle()        { return { ActionKind::Idle,    Dir::N }; }
  static constexpr Action grow(Dir d)   { return { ActionKind::Grow,    d      }; } // energy 2: claim an open neighbour at strength 1
  static constexpr Action fortify()     { return { ActionKind::Fortify, Dir::N }; } // energy 1: +1 to your own strength (max 4)
  static constexpr Action attack(Dir d) { return { ActionKind::Attack,  d      }; } // energy 1: -1 to an enemy neighbour's strength
  static constexpr Action move(Dir d)   { return { ActionKind::Move,    d      }; } // energy 1: step into an open neighbour, keeping your strength and energy
  static constexpr Action spore(Dir d)  { return { ActionKind::Spore,   d      }; } // energy 6: seed a cell THREE steps away, leaping whatever is between

  // Energy this action costs. Grow is double because area is how you score --
  // Move is half of it precisely because moving gains you no ground. Spore is
  // triple Grow because it can cross ground you do not control -- far too
  // expensive to expand with, so it is purely a way through a blockade.
  //
  // A switch rather than a chain of ?: on purpose: ActionKind and Cost are two
  // lists that have to agree, and a switch is the shape a compiler can check --
  // add a kind without pricing it and -Wswitch names this spot. That needs
  // -Wall, which platformio.ini does not set today; until it does, an unpriced
  // action silently costs nothing.
  constexpr uint8_t cost() const {
    switch (kind) {
      case ActionKind::Grow:    return static_cast<uint8_t>(Cost::Grow);
      case ActionKind::Fortify: return static_cast<uint8_t>(Cost::Fortify);
      case ActionKind::Attack:  return static_cast<uint8_t>(Cost::Attack);
      case ActionKind::Move:    return static_cast<uint8_t>(Cost::Move);
      case ActionKind::Spore:   return static_cast<uint8_t>(Cost::Spore);
      case ActionKind::Idle:    break;
    }
    return static_cast<uint8_t>(Cost::Idle);
  }
};

// A cell's durability: how many attacks it takes to kill it. Fortify raises it
// (up to Strongest); each Attack lowers it; a cell that would drop to None dies.
// The levels are ordered, so you can compare them directly:
//   if (enemy.strength < me.strength) ...   // enemy is softer than me
// Mixed case on purpose -- some all-caps names are reserved by the Arduino core.
enum class Strength : uint8_t { None = 0, Weak = 1, Normal = 2, Strong = 3, Strongest = 4 };

// A neighbouring tile as your cell sees it.
struct Neighbour {
  Occupant occupant;
  Strength strength;   // Weak..Strongest when Self or Enemy; None otherwise
  uint8_t  enemy_id;   // stable per-match id of the enemy (1..4); 0 unless Enemy
  uint8_t  note;       // reserved for a future level; always 0 in v1

  bool isEmpty()   const { return occupant == Occupant::Empty;   }
  bool isNeutral() const { return occupant == Occupant::Neutral; }
  bool isSelf()    const { return occupant == Occupant::Self;    }
  bool isEnemy()   const { return occupant == Occupant::Enemy;   }
  bool isWall()    const { return occupant == Occupant::Wall;    }
  bool isEdge()    const { return occupant == Occupant::Edge;    }
};

// Your cell: its own strength, its own purse, and its four neighbours.
struct Cell {
  Strength  strength;    // your durability here, Weak..Strongest
  uint16_t  energy;      // THIS CELL's banked energy -- see canAfford* below
  uint8_t   note;        // reserved for a future level; always 0 in v1
  Neighbour n[4];        // indexed by Dir: n[0]=N, n[1]=E, n[2]=S, n[3]=W
  bool      far_open[4]; // is the tile THREE steps this way free to Spore into?
                         // (all you sense at range -- use canSpore() below)

  const Neighbour& neighbour(Dir d) const { return n[(uint8_t)d]; }

  // ---- can this cell PAY for it? (money only -- ignores terrain) -----------
  //
  // Exact and binding, not a hint: energy belongs to this cell alone, so when
  // one of these says true the action WILL be funded -- no other cell can spend
  // the money first. Ask anyway when it says false and the action is simply
  // skipped: this cell keeps its energy and none of your other cells are
  // affected, so it costs you nothing but this cell's tick.
  //
  // These are the ones to reach for when a cell should SAVE. Return
  // Action::idle() while canAffordGrow() is false and the cell banks toward the
  // 2; spend the 1 it has on a Fortify instead and it may never get there.
  bool canAffordGrow()    const { return energy >= static_cast<uint8_t>(Cost::Grow);    }
  bool canAffordFortify() const { return energy >= static_cast<uint8_t>(Cost::Fortify); }
  bool canAffordAttack()  const { return energy >= static_cast<uint8_t>(Cost::Attack);  }
  bool canAffordMove()    const { return energy >= static_cast<uint8_t>(Cost::Move);    }
  bool canAffordSpore()   const { return energy >= static_cast<uint8_t>(Cost::Spore);   }

  // ---- can this cell actually DO it? (money AND terrain) -------------------
  //
  // Each of these folds in the matching canAfford* above, so a false answer can
  // mean either "cannot pay yet" or "nowhere to do it". When you need to tell
  // those two apart, ask canAfford*() and find*() separately.

  // True if you could Grow this way right now: you can pay the 2 and the
  // neighbour is open ground.
  bool canGrow(Dir d) const {
    const Neighbour& q = neighbour(d);
    return canAffordGrow() && (q.isEmpty() || q.isNeutral());
  }

  // True if you could Move this way right now: you can pay the 1 and the
  // neighbour is open ground -- a cell walks onto exactly the ground it could
  // have claimed, for half the price of claiming it.
  bool canMove(Dir d) const {
    const Neighbour& q = neighbour(d);
    return canAffordMove() && (q.isEmpty() || q.isNeutral());
  }

  // True if you could Spore this way right now: you can pay the 6 and the tile
  // THREE steps away is on the board and open. Whatever sits between you and it
  // does not matter -- that is the point of Spore, it leaps over enemies, your
  // own cells, walls.
  bool canSpore(Dir d) const { return canAffordSpore() && far_open[(uint8_t)d]; }

  // True if you could Fortify right now: you can pay the 1 and you are not
  // already Strongest.
  bool canFortify() const { return (strength < Strength::Strongest) && canAffordFortify(); }

  // True if you could Attack this way right now: you can pay the 1 and there is
  // an enemy in that direction.
  bool canAttack(Dir d) const { return canAffordAttack() && neighbour(d).isEnemy(); }

  // ---- convenience helpers (optional; write raw loops if you prefer) --------

  // How many of the 4 neighbours are of a given kind, e.g.
  // countAdjacent(Occupant::Enemy).
  int countAdjacent(Occupant o) const {
    int c = 0;
    for (uint8_t d = 0; d < 4; d++) if (n[d].occupant == o) c++;
    return c;
  }

  // ---- WHERE DO I LOOK FIRST? ----------------------------------------------
  //
  // Every find* helper below scans all four neighbours and stops at the first
  // one that qualifies, so when two directions both qualify the SCAN ORDER
  // decides. Leave `from` alone and that order is always N,E,S,W, which means a
  // cell in open ground grows north every single time and a whole virus drifts
  // north-east.
  //
  // That drift is not just untidy, it is unfair: it breaks the board's own
  // symmetry. Four identical viruses in four corners are NOT in equivalent
  // positions if all of them prefer north -- the one in the north-west is
  // against the rim after one step while the one in the south-east has the
  // whole board ahead of it.
  //
  // Pass `from` to start the scan somewhere else. Handing it world.tick walks
  // the preferred direction round the compass, so over any four ticks each
  // direction is favoured once and growth spreads evenly instead of leaning:
  //
  //     me.findOpen(d, (Dir)(world.tick & 3))
  //
  // The default is the old N,E,S,W order, so rules that ignore `from` behave
  // exactly as they always did -- including the drift, which is left in as
  // something to notice and fix.

  // First open-ground neighbour (Empty or Neutral). Returns true and sets `out`
  // to a direction you can Grow or Move into; false if you are boxed in.
  bool findOpen(Dir& out, Dir from = Dir::N) const {
    for (uint8_t i = 0; i < 4; i++) {
      const uint8_t d = ((uint8_t)from + i) & 3;
      if (n[d].isEmpty() || n[d].isNeutral()) { out = (Dir)d; return true; }
    }
    return false;
  }

  // First direction you could Spore into (open tile three steps away). Returns
  // true and sets `out`; false if every landing spot is blocked or off-board.
  bool findSporeTarget(Dir& out, Dir from = Dir::N) const {
    for (uint8_t i = 0; i < 4; i++) {
      const uint8_t d = ((uint8_t)from + i) & 3;
      if (far_open[d]) { out = (Dir)d; return true; }
    }
    return false;
  }

  // Weakest neighbouring enemy. Returns true and sets `out` to its direction;
  // false if no enemy is adjacent. Equally weak enemies are settled by the scan
  // order, so `from` decides those the same way it decides an open field.
  bool findWeakestEnemy(Dir& out, Dir from = Dir::N) const {
    bool found = false;
    Strength best = Strength::Strongest;
    for (uint8_t i = 0; i < 4; i++) {
      const uint8_t d = ((uint8_t)from + i) & 3;
      if (!n[d].isEnemy()) continue;
      if (!found || n[d].strength < best) { best = n[d].strength; out = (Dir)d; found = true; }
    }
    return found;
  }

  // Strongest neighbouring enemy. Returns true and sets `out` to its direction;
  // false if no enemy is adjacent. Ties settled by the scan order, as above.
  bool findStrongestEnemy(Dir& out, Dir from = Dir::N) const {
    bool found = false;
    Strength best = Strength::None;
    for (uint8_t i = 0; i < 4; i++) {
      const uint8_t d = ((uint8_t)from + i) & 3;
      if (!n[d].isEnemy()) continue;
      if (!found || n[d].strength > best) { best = n[d].strength; out = (Dir)d; found = true; }
    }
    return found;
  }
};

// How far along the match is. Blended from how much time has elapsed AND how
// full the board has become -- whichever says the game is further along -- so
// it tracks the real pace of play (a match that fills up fast reaches End
// sooner than the clock alone would). Lets a rule shift strategy over the
// course of a game without knowing anything about the board globally.
enum class Phase : uint8_t { Early, Middle, End };

// One device's button A, as System::Button reports it: a LIVE LEVEL, sampled
// once per referee tick, not an edge. Holding the button down reads true on
// every tick for as long as it stays held, not just the tick it was pressed --
// a rule that wants a one-shot trigger has to watch for the false-to-true
// transition itself (WITHOUT remembering it: see the note on World above,
// this is still just a value handed in fresh each call).
//
// In a single-device match all four viruses share the one physical button, so
// every rule that reads it sees the same press at the same time -- there is
// only one button on the board to read from. In networked play it is each
// device's own button, local to the one virus that device is running.
struct ButtonState {
  bool held;
  bool pressed() const { return held; }
};

// Match facts your cell sees this tick -- the same for every cell of every
// virus. Everything that is yours alone (your strength, your energy) lives on
// `me`, which is what keeps a rule reasoning locally.
struct World {
  Phase       phase;
  ButtonState button;   // button A -- see ButtonState above

  // Ticks since the match began. Like everything else here it is handed in
  // fresh each call, not something your rule remembers -- and both ends of a
  // networked match hold the same number when their rules run, so using it
  // cannot pull two devices out of step.
  //
  // Mostly useful as the `from` argument to the find* helpers: it walks the
  // preferred scan direction round the compass and takes the north-east drift
  // out of a rule built on them. See WHERE DO I LOOK FIRST above.
  uint16_t    tick;
};

// What your rule returns. An Action converts to one on its own, so the usual
// shape is simply:
//   return Action::grow(d);
// `note` is reserved for a future level and ignored in v1.
struct Decision {
  Action  action;
  uint8_t note;

  Decision(Action a, uint8_t n = 0) : action(a), note(n) {}
};

// A virus is just a function pointer of this shape.
using RuleFn = Decision (*)(const Cell& me, const World& world);
