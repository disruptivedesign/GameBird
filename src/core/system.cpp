#include "system.h"

void System::begin(){
  // FIRST, before any peripheral that takes time to come up. The buzzer FET gate
  // must never float, and audio.begin() cannot run until storage is up -- which
  // is after the display and the radio. The gate is active low, so an unclaimed
  // pin fails toward ON. See Audio::parkPin for what that gap costs.
  Audio::parkPin();

  // Also early: the joystick pots are powered from this pin rather than the
  // 3.3V rail directly. Drive it HIGH now so they have the rest of begin() to
  // settle before joy.begin() samples them for centre; it is never touched
  // again after this.
  pinMode(JOY_PWR_PIN, OUTPUT);
  digitalWrite(JOY_PWR_PIN, HIGH);

  analogReadResolution(12);          // 0..4095, before the joystick samples centre
  display.begin();
  buttonA.begin(BTN_A_PIN);
  buttonB.begin(BTN_B_PIN);
  joyButton.begin(JOY_BTN_PIN);      // reserved for future use -- not wired to any action yet
  // After analogReadResolution and deliberately before anything slow: begin()
  // samples the stick's resting position to use as centre, so it wants to run
  // while the player is still holding the unit rather than the stick.
  joy.begin();
  storage.begin();
  net.begin(&storage);
  audio.begin(&storage);   // after storage: it loads the saved volume
}

void System::update(){
  uint32_t now = millis();
  buttonA.update(now);
  buttonB.update(now);
  joyButton.update(now);
  joy.update(now);
  net.update(now);
  audio.update(now);
}
