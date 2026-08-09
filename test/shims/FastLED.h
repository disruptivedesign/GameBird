#pragma once
// ============================================================================
//  Minimal FastLED shim for the native (PC) test build.
//
//  Only the colour type and the two blend helpers the games call. No drivers,
//  no timing, no hardware -- rendering is stubbed out in the test suite, so
//  this exists to make the render paths compile and link, not to produce
//  pixels.
//
//  Native env only (-I test/shims); the ESP32 builds use the real FastLED.
// ============================================================================
#include <stdint.h>

struct CRGB {
  uint8_t r, g, b;

  CRGB() : r(0), g(0), b(0) {}
  constexpr CRGB(uint8_t red, uint8_t green, uint8_t blue) : r(red), g(green), b(blue) {}

  // Matches FastLED: CRGB::Black and friends are packed 0xRRGGBB codes that
  // convert through this constructor.
  constexpr CRGB(uint32_t code)
    : r((uint8_t)((code >> 16) & 0xFF)),
      g((uint8_t)((code >> 8) & 0xFF)),
      b((uint8_t)(code & 0xFF)) {}

  enum HTMLColorCode : uint32_t {
    Black = 0x000000,
    White = 0xFFFFFF,
    Red   = 0xFF0000,
    Green = 0x008000,
    Blue  = 0x0000FF,
  };

  // Scale every channel by s/255.
  CRGB& nscale8(uint8_t s) {
    r = (uint8_t)((uint16_t)r * (uint16_t)(s + 1) >> 8);
    g = (uint8_t)((uint16_t)g * (uint16_t)(s + 1) >> 8);
    b = (uint8_t)((uint16_t)b * (uint16_t)(s + 1) >> 8);
    return *this;
  }

  // As nscale8, but a channel that was lit never scales all the way to zero.
  // FastLED's "video" variants exist so a dimmed colour keeps its hue instead
  // of dropping channels one at a time as it fades out.
  CRGB& nscale8_video(uint8_t s) {
    const uint8_t nz = (s == 0) ? 0 : 1;
    r = (uint8_t)(((uint16_t)r * (uint16_t)s) >> 8) + ((r && s) ? nz : 0);
    g = (uint8_t)(((uint16_t)g * (uint16_t)s) >> 8) + ((g && s) ? nz : 0);
    b = (uint8_t)(((uint16_t)b * (uint16_t)s) >> 8) + ((b && s) ? nz : 0);
    return *this;
  }

  bool operator==(const CRGB& o) const { return r == o.r && g == o.g && b == o.b; }
};

// Linear blend from a to b, amount 0 = all a, 255 = all b.
inline CRGB blend(const CRGB& a, const CRGB& b, uint8_t amount) {
  return CRGB(
    (uint8_t)(a.r + (((int)b.r - (int)a.r) * amount) / 255),
    (uint8_t)(a.g + (((int)b.g - (int)a.g) * amount) / 255),
    (uint8_t)(a.b + (((int)b.b - (int)a.b) * amount) / 255));
}
