#pragma once
#include "core/game.h"
#include "core/system.h"

// ============================================================================
//  Settings: a top-level menu entry with two pages -- User Color and Volume.
//
//  ONE state, not three. Rev 1 needed a submenu and an enter/exit step per page
//  because a slider has a single axis: picking a page and picking a value both
//  wanted the same control. The joystick has two, so left/right picks the page
//  and up/down changes that page's value in place. There is nothing to enter.
//
//  Both values PREVIEW live and commit on the way out: A saves, B reverts. That
//  is what keeps the volume page usable -- an unheard volume control is
//  useless, so each step has to be audible immediately, and it can only be free
//  to experiment with if backing out puts it back.
// ============================================================================
class Settings : public Game {
public:
  Settings(System& sys) : _sys(sys) {}
  void       begin() override;
  GameStatus service() override;
  Icon       menuIcon() const override;

private:
  System&  _sys;
  Display& disp() { return _sys.display; }

  uint8_t _page = 0;                   // 0 = User Color, 1 = Volume

  uint8_t _colorSel = 0;               // previewed, not yet persisted
  uint8_t _colorBefore = 0;
  uint8_t _volSel = VOL_MED;
  Volume  _volBefore = VOL_MED;

  bool    _wantExit = false;

  void adjust(int8_t step);            // up/down on the current page
  void commit();
  void revert();

  void render();
  void drawColorPage();
  void drawVolumePage();
  void drawVolumeMeter(uint8_t lvl);
};
