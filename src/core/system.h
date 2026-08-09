#pragma once
#include "display.h"
#include "controls.h"
#include "storage.h"
#include "net/net.h"
#include "audio/audio.h"

// ============================================================================
//  System: owns the hardware peripherals and is passed by reference into
//  games. update() polls all inputs once per tick.
// ============================================================================
class System {
public:
  Display  display;
  Button   buttonA, buttonB, joyButton;
  Joystick joy;
  Storage  storage;
  Net      net;
  Audio    audio;

  void begin();
  void update();     // poll all inputs once per tick
};
