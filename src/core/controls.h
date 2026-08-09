#pragma once
#include <Arduino.h>
#include "defines.h"

// ============================================================================
//  Agnostic input devices. Poll each once per tick via update(); games then
//  read the cached state. No game-specific meaning lives here.
// ============================================================================

// Four-way direction in SCREEN space: DIR_UP means "toward row 0". See the note
// on Joystick below -- this enum exists specifically so that games never handle
// the stick-to-screen sign flip themselves.
enum JoyDir : uint8_t { DIR_NONE = 0, DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

// Debounced push button (active low, internal pull-up).
class Button {
public:
  void begin(uint8_t pin);
  void update(uint32_t now);          // call once per tick
  bool wasPressed()  const { return _pressedEdge; }
  bool wasReleased() const { return _releasedEdge; }
  bool isHeld()      const { return _stable == LOW; }
private:
  uint8_t  _pin = 0;
  bool     _stable = HIGH, _last = HIGH;
  bool     _pressedEdge = false, _releasedEdge = false;
  uint32_t _tChange = 0;
};

// One analog joystick axis -> signed percentage. -100 = full one way, 0 =
// centered, +100 = full the other. Handles smoothing, inversion, a boot-sampled
// centre and a rescaled centre deadzone.
class Axis {
public:
  void begin(uint8_t pin, bool invert);
  void update();                      // call once per tick
  int  read()      const { return _percent; }   // -100..+100
  int  centerRaw() const { return _center; }    // diagnostics / tests
private:
  uint8_t _pin = 0;
  bool    _invert = false;
  int     _center   = ADC_MAX / 2;
  int     _smoothed = ADC_MAX / 2;
  int     _percent  = 0;
};

// ============================================================================
//  Two-axis analog joystick, offering the same stick two different ways.
//
//  PROPORTIONAL -- x() and y(). For a quantity that is itself continuous: how
//  fast Breakout's paddle should be travelling. Reading these is how you use
//  the fact that this is a stick and not a d-pad.
//
//  STEPPED -- stepX() and stepY(). One event per deflection, then auto-repeat
//  after a hold delay, with hysteresis so resting near the threshold cannot
//  chatter. This is what every menu wants. It is also the thing rev 1's slider
//  could not do: a pot maps position to index, and a self-centering stick reads
//  zero at rest, so a ported position-to-index selector sits on its middle
//  entry forever.
//
//  ---- On the Y sign ------------------------------------------------------
//  The hardware's +Y is UP. The panel's rows grow DOWN. That flip is applied
//  exactly once, here, on the way out:
//
//      y()      is STICK space  -- positive means the stick is pushed up
//      stepY()  is SCREEN space -- positive means "move toward a higher row"
//      dir()    is SCREEN space -- DIR_UP means "toward row 0"
//
//  y() stays in stick space because that is what JOY_INVERT_Y is calibrated
//  against and what a human reasons about when tuning. Games should consume
//  dir() and stepY() and never the raw sign of y(), so the flip lives in one
//  place and is pinned by test_controls.
// ============================================================================
class Joystick {
public:
  void begin();
  void update(uint32_t now);          // call once per tick

  int  x() const { return _ax.read(); }    // -100..+100, + = right
  int  y() const { return _ay.read(); }    // -100..+100, + = up  (stick space)

  int8_t stepX() const { return _stepX; }  // -1/0/+1 this tick, + = right
  int8_t stepY() const { return _stepY; }  // -1/0/+1 this tick, + = DOWN a row
  JoyDir dir()   const;                    // held, dominant axis, screen space

  // Test/diagnostic access to the underlying axes.
  const Axis& axisX() const { return _ax; }
  const Axis& axisY() const { return _ay; }

private:
  // Turns a continuous deflection into discrete steps: one on the way past
  // JOY_STEP_ON, then a repeat every JOY_REPEAT_MS once JOY_REPEAT_DELAY_MS has
  // elapsed. Rearms only after falling back inside JOY_STEP_OFF.
  struct Stepper {
    int8_t   latched = 0;             // 0 = idle, else the armed direction
    uint32_t nextAt  = 0;
    int8_t   step(int pct, uint32_t now);
  };

  Axis    _ax, _ay;
  Stepper _sx, _sy;
  int8_t  _stepX = 0, _stepY = 0;
};
