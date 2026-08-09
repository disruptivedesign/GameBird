#include "controls.h"

// ---- Button ----------------------------------------------------------------
void Button::begin(uint8_t pin){
  _pin = pin;
  pinMode(pin, INPUT_PULLUP);
  _stable = _last = HIGH;
  _pressedEdge = _releasedEdge = false;
  _tChange = 0;
}

void Button::update(uint32_t now){
  _pressedEdge = _releasedEdge = false;      // edges are valid for one tick only
  bool raw = digitalRead(_pin);              // HIGH = released, LOW = pressed
  if (raw != _last){ _last = raw; _tChange = now; }
  if (now - _tChange >= DEBOUNCE_MS && raw != _stable){
    _stable = raw;
    if (_stable == LOW) _pressedEdge = true;
    else                _releasedEdge = true;
  }
}

// ---- Axis ------------------------------------------------------------------
void Axis::begin(uint8_t pin, bool invert){
  _pin = pin;
  _invert = invert;

  // Sample the resting position rather than assuming mid-scale. A thumbstick
  // rarely rests at exactly ADC_MAX/2, and treating mid-scale as centre gives
  // one direction more usable travel than the other -- which shows up as a
  // paddle that drifts on its own with nobody touching the stick.
  long acc = 0;
  for (int i = 0; i < 16; i++) acc += analogRead(pin);
  int c = (int)(acc / 16);

  // Only trust it if it looks like a centered stick. If the stick happens to be
  // held at boot this reads an extreme, and adopting that would put the true
  // centre at full deflection forever. Falling back to mid-scale is wrong by a
  // little instead of wrong by everything.
  const int mid = ADC_MAX / 2, tol = ADC_MAX / 5;    // +/- 20%
  _center   = (c > mid - tol && c < mid + tol) ? c : mid;
  _smoothed = c;
}

void Axis::update(){
  _smoothed = (_smoothed * 3 + analogRead(_pin)) / 4;         // EMA
  int raw = _invert ? (ADC_MAX - _smoothed) : _smoothed;
  int ctr = _invert ? (ADC_MAX - _center)   : _center;

  // Each side of centre is scaled by its OWN span. Centre is rarely mid-scale,
  // so the two sides have different numbers of counts; one divisor for both
  // would make the short side saturate before full deflection and stop the long
  // side ever reaching 100.
  //
  // ROUNDED, not truncated, and the rescale below is too. Two truncations
  // compound: the EMA settles one count shy of its target, that reads as 99
  // instead of 100, and the deadzone rescale turns 99 into 98 -- so full
  // physical travel never reports full deflection. Harmless in a menu, not
  // harmless for a velocity-driven paddle whose top speed becomes unreachable.
  int pct;
  if (raw >= ctr){
    int span = ADC_MAX - ctr;
    pct = span > 0 ?  (int)(((long)(raw - ctr) * 100 + span / 2) / span) : 0;
  } else {
    int span = ctr;
    pct = span > 0 ? -(int)(((long)(ctr - raw) * 100 + span / 2) / span) : 0;
  }
  pct = constrain(pct, -100, 100);

  // Centre deadzone, RESCALED rather than flattened. Collapsing the middle band
  // to zero and leaving the rest alone silently costs 2*JOY_DEADZONE percent of
  // physical travel. On rev 1 that showed up as Breakout's paddle snagging as it
  // crossed centre; with a velocity-driven paddle it would show up as speed
  // jumping from nothing straight to visibly moving. Stretching the live band
  // back out to the full range keeps the same jitter immunity at centre and
  // costs nothing anywhere else.
  const int dz = JOY_DEADZONE;
  if (pct > -dz && pct < dz){
    pct = 0;
  } else {
    int mag = pct < 0 ? -pct : pct;
    mag = ((mag - dz) * 100 + (100 - dz) / 2) / (100 - dz);
    pct = pct < 0 ? -mag : mag;
  }
  _percent = pct;
}

// ---- Joystick --------------------------------------------------------------
int8_t Joystick::Stepper::step(int pct, uint32_t now){
  const int  mag  = pct < 0 ? -pct : pct;
  const int8_t sg = pct > 0 ? 1 : (pct < 0 ? -1 : 0);

  if (latched == 0){
    if (mag < JOY_STEP_ON) return 0;
    latched = sg;
    nextAt  = now + JOY_REPEAT_DELAY_MS;
    return sg;                                  // the first step is immediate
  }

  // Rearm on falling back inside the hysteresis band, or on a flick straight
  // across to the other side without passing through it. Either way the next
  // tick is free to latch again, so a reversal costs one tick, not a return to
  // centre.
  if (mag < JOY_STEP_OFF || sg != latched){
    latched = 0;
    return 0;
  }

  // Signed compare so this is immune to millis() wrapping.
  if ((int32_t)(now - nextAt) >= 0){
    nextAt = now + JOY_REPEAT_MS;
    return latched;
  }
  return 0;
}

void Joystick::begin(){
  _ax.begin(JOY_X_PIN, JOY_INVERT_X);
  _ay.begin(JOY_Y_PIN, JOY_INVERT_Y);
  _sx = Stepper();
  _sy = Stepper();
  _stepX = _stepY = 0;
}

void Joystick::update(uint32_t now){
  _ax.update();
  _ay.update();
  _stepX =  _sx.step(_ax.read(), now);
  // Negated: the stepper works in stick space (+ = up) and stepY is a screen
  // step (+ = down a row). This is the one place that flip happens.
  _stepY = (int8_t)(-_sy.step(_ay.read(), now));
}

JoyDir Joystick::dir() const {
  const int ax = x(), ay = y();
  const int mx = ax < 0 ? -ax : ax;
  const int my = ay < 0 ? -ay : ay;

  if (mx < JOY_STEP_ON && my < JOY_STEP_ON) return DIR_NONE;
  if (mx == my) return DIR_NONE;      // exact diagonal: no request, keep going
  if (mx > my)  return ax > 0 ? DIR_RIGHT : DIR_LEFT;
  return ay > 0 ? DIR_UP : DIR_DOWN;  // stick +Y is up; DIR_UP is toward row 0
}
