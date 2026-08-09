#pragma once
#include "core/display.h"
#include "audio/audio.h"

// ============================================================================
//  Boot splash / attract animation. Independent of any game so it can run at
//  system boot. Blocking: play() runs the animation once and returns.
//
//  Because it blocks, it has to pump Audio itself -- System::update() is not
//  running yet at boot, and an un-pumped sound would hold its first note for
//  the whole animation instead of advancing through it.
// ============================================================================
class Splash {
public:
  Splash(Display& d, Audio& a) : _d(d), _a(a) {}
  void play();
private:
  Display& _d;
  Audio&   _a;
};
