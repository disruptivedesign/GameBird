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
  constexpr uint8_t cost() const {
    return kind == ActionKind::Grow    ? 2
         : kind == ActionKind::Fortify ? 1
         : kind == ActionKind::Attack  ? 1
         : kind == ActionKind::Move    ? 1
         : kind == ActionKind::Spore   ? 6
         : 0;   // Idle
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
  uint16_t  energy;      // THIS CELL's banked energy -- see canAfford() below
  uint8_t   note;        // reserved for a future level; always 0 in v1
  Neighbour n[4];        // indexed by Dir: n[0]=N, n[1]=E, n[2]=S, n[3]=W
  bool      far_open[4]; // is the tile THREE steps this way free to Spore into?
                         // (all you sense at range -- use canSpore() below)

  const Neighbour& neighbour(Dir d) const { return n[(uint8_t)d]; }

  // Can this cell pay for that action right now? Unlike v1's shared purse, the
  // answer is EXACT and binding: energy belongs to this cell alone, so if this
  // returns true the action WILL be funded. Nothing else can spend it first.
  // If it returns false the action is simply skipped -- no other cell of yours
  // is affected, so an unaffordable request costs you nothing but this cell's
  // tick. Return Action::idle() to save up instead.
  bool canAfford(const Action& a) const { return energy >= a.cost(); }

  // True if you could Grow in this direction (neighbour is open ground).
  bool canGrow(Dir d) const {
    const Neighbour& q = neighbour(d);
    return q.isEmpty() || q.isNeutral();
  }

  // True if you could Move in this direction. Same test as canGrow: a cell
  // walks onto exactly the ground it could have claimed.
  bool canMove(Dir d) const { return canGrow(d); }

  // True if you could Spore in this direction: the tile THREE steps away is on
  // the board and open. Whatever sits between you and it does not matter --
  // that is the point of Spore, it leaps over enemies, your own cells, walls.
  bool canSpore(Dir d) const { return far_open[(uint8_t)d]; }

  // ---- convenience helpers (optional; write raw loops if you prefer) --------

  // How many of the 4 neighbours are of a given kind, e.g.
  // countAdjacent(Occupant::Enemy).
  int countAdjacent(Occupant o) const {
    int c = 0;
    for (uint8_t d = 0; d < 4; d++) if (n[d].occupant == o) c++;
    return c;
  }

  // First open-ground neighbour (Empty or Neutral). Returns true and sets `out`
  // to a direction you can Grow or Move into; false if you are boxed in.
  bool findOpen(Dir& out) const {
    for (uint8_t d = 0; d < 4; d++)
      if (n[d].isEmpty() || n[d].isNeutral()) { out = (Dir)d; return true; }
    return false;
  }

  // First direction you could Spore into (open tile three steps away). Returns
  // true and sets `out`; false if every landing spot is blocked or off-board.
  bool findSporeTarget(Dir& out) const {
    for (uint8_t d = 0; d < 4; d++)
      if (far_open[d]) { out = (Dir)d; return true; }
    return false;
  }

  // Weakest neighbouring enemy. Returns true and sets `out` to its direction;
  // false if no enemy is adjacent. Ties pick the first in N,E,S,W order.
  bool findWeakestEnemy(Dir& out) const {
    bool found = false;
    Strength best = Strength::Strongest;
    for (uint8_t d = 0; d < 4; d++) {
      if (!n[d].isEnemy()) continue;
      if (!found || n[d].strength < best) { best = n[d].strength; out = (Dir)d; found = true; }
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
