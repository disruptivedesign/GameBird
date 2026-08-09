// ============================================================================
//  Hardware stubs shared by every native suite.
//
//  Virus::render() is the only part of the referee that touches a Display, and
//  the sim never reads anything back from it, so the panel is a no-op here.
//  Everything the tests assert on comes from serializeState().
//
//  The fake pins exist for core/controls.cpp: test_controls drives a joystick
//  and a pair of buttons that are not attached to anything.
// ============================================================================
#include <Arduino.h>
#include "core/display.h"
#include "audio/audio.h"

SerialShim Serial;

static unsigned long g_millis = 0;
unsigned long millis()                     { return g_millis; }
void nativeAdvanceMillis(unsigned long ms) { g_millis += ms; }

// ---- Fake pins --------------------------------------------------------------
#define NATIVE_PINS 64

static int g_analog[NATIVE_PINS];
static int g_digital[NATIVE_PINS];
static bool g_pinsInit = false;

static void ensurePins(){
  if (g_pinsInit) return;
  for (int i = 0; i < NATIVE_PINS; i++){
    g_analog[i]  = 2048;    // mid-scale: a centered stick
    g_digital[i] = HIGH;    // released, matching an idle INPUT_PULLUP button
  }
  g_pinsInit = true;
}

void nativeResetPins(){ g_pinsInit = false; ensurePins(); }

void nativeSetAnalog(uint8_t pin, int value){
  ensurePins();
  if (pin < NATIVE_PINS) g_analog[pin] = value;
}

void nativeSetDigital(uint8_t pin, int value){
  ensurePins();
  if (pin < NATIVE_PINS) g_digital[pin] = value;
}

void pinMode(uint8_t, uint8_t) {}
void digitalWrite(uint8_t, int) {}
void analogReadResolution(uint8_t) {}

int digitalRead(uint8_t pin){
  ensurePins();
  return pin < NATIVE_PINS ? g_digital[pin] : HIGH;
}

int analogRead(uint8_t pin){
  ensurePins();
  return pin < NATIVE_PINS ? g_analog[pin] : 2048;
}

// ---- Display ----------------------------------------------------------------
// Only the Display methods the natively-compiled render paths call need to link.
void Display::clear() {}
void Display::setPixel(int, int, const CRGB&) {}
void Display::border(const CRGB&) {}

// ---- Audio ------------------------------------------------------------------
// Swarm's NetGame wrapper turns sim events into sounds on the way past. Nothing
// asserts on audio, but the calls have to resolve for the suite to link -- and
// compiling that file is what puts Swarm::aiInput, the solo wingman, under test
// at all.
void Audio::play(const Sfx&) {}
