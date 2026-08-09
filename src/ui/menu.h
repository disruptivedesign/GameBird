#pragma once
#include "core/system.h"
#include "core/game.h"

// ============================================================================
//  Top-level game selector. One full-screen icon per page; the joystick steps
//  left/right through them and a button launches the entry's game.
//  Non-launchable entries (placeholder / settings) have game == nullptr and do
//  nothing on press.
//
//  Selection is RELATIVE, which it could not be on rev 1: a pot maps position
//  to index, so the page was wherever the slider physically sat. A self-
//  centering stick reads zero at rest, so the page is state this owns.
//
//  That loses the one thing the slider gave for free -- knowing where you are in
//  the list without pressing anything. Two things replace it: the icons SLIDE in
//  the direction you stepped, and a position strip appears along the bottom and
//  fades out again. The strip is deliberately temporary rather than permanent,
//  the same call Breakout makes with its lives: show it when it is the thing
//  being asked about, not always.
// ============================================================================
struct MenuEntry {
  Game* game;   // nullptr => non-launchable; icon taken from `icon` below
  Icon  icon;   // used only when game == nullptr
};

class Menu {
public:
  Menu(System& sys, MenuEntry* entries, int count)
    : _sys(sys), _entries(entries), _count(count) {}
  void begin();
  Game* service();     // returns the chosen Game* once picked, else nullptr

private:
  System&    _sys;
  MenuEntry* _entries;
  int        _count;
  int        _page = 0;

  int        _prevPage  = 0;   // the icon sliding out
  int8_t     _slideDir  = 0;   // 0 = settled, else the direction stepped
  uint32_t   _slideStart = 0;
  uint32_t   _strokeAt   = 0;  // last step, for the strip's fade

  Icon iconFor(int i);
  void render(uint32_t now);
  void drawStrip(uint32_t now);
};
