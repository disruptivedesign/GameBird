#pragma once
#include <FastLED.h>
#include "defines.h"

// ============================================================================
//  Abstract game interface. Every game implements begin(), service(), and
//  provides a top-level menu icon. A top-level menu holds a list of Game* and
//  switches between them.
// ============================================================================

enum GameStatus {
  GAME_CONTINUE,   // keep servicing this game
  GAME_EXIT,       // game wants to hand control back (future: to a top menu)
};

// Menu icon: a bitmap of palette codes plus the palette (code 0 = transparent).
// Authored at ICON_W x ICON_H on every panel; Display::drawBitmap scales it.
struct Icon {
  const uint8_t (*bmp)[ICON_W];
  const CRGB*    pal;
};

class Game {
public:
  virtual void begin() = 0;             // one-time setup
  virtual GameStatus service() = 0;     // one non-blocking tick
  virtual Icon menuIcon() const = 0;    // identity in the top-level menu

  // Called by the shell whenever control leaves this game -- on its own
  // GAME_EXIT and on a forced quit alike. Default is nothing, because most
  // games hold no resource worth releasing; override it if yours does (see
  // Multiplayer, which must drop its lobby or keep beaconing to nobody).
  virtual void end() {}

  virtual ~Game() {}
};
