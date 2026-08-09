#pragma once
// ============================================================================
//  Minimal Arduino shim for the native (PC) test build.
//
//  Two things reach for the Arduino core from the natively-compiled sources:
//
//    - The Virus referee's include chain wants fixed-width types and, when
//      telemetry is on, Serial. The native env builds with -DVIRUS_TELEMETRY=0,
//      so Serial here exists only to keep any stray reference linking.
//    - core/controls.cpp wants the GPIO and ADC calls. Those are backed by
//      settable fake pins (see nativeSetAnalog / nativeSetDigital in stubs.cpp)
//      so test_controls can drive a joystick that does not exist.
//
//  This file is on the include path for the native env ONLY (-I test/shims).
//  The ESP32 builds never see it; they get the real Arduino core.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// Milliseconds since start. The sims never read this for logic -- only render()
// paths use it, to time flashes -- so a simple counter is enough.
unsigned long millis();

// Advance the fake clock. Test-only; lets a test step past a flash window.
void nativeAdvanceMillis(unsigned long ms);

// ---- GPIO / ADC -------------------------------------------------------------
#define HIGH          1
#define LOW           0
#define INPUT         0
#define OUTPUT        1
#define INPUT_PULLUP  2

void pinMode(uint8_t pin, uint8_t mode);
int  digitalRead(uint8_t pin);
void digitalWrite(uint8_t pin, int value);
int  analogRead(uint8_t pin);
void analogReadResolution(uint8_t bits);

// Test-only: drive the fake pins. Every pin defaults to HIGH (released, matching
// an idle INPUT_PULLUP button) and mid-scale on the ADC.
void nativeSetAnalog(uint8_t pin, int value);
void nativeSetDigital(uint8_t pin, int value);
void nativeResetPins();

// Arduino's own definition, macro included -- callers pass mixed integer types
// and a template would reject them.
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))

struct SerialShim {
  void begin(unsigned long) {}
  int  printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
  }
  void print(const char* s)   { fputs(s, stdout); }
  void println(const char* s) { puts(s); }
  void println()              { putchar('\n'); }
};

extern SerialShim Serial;
