#pragma once
#include <Arduino.h>
#include "core/defines.h"
#include "sound.h"

class Storage;

// ============================================================================
//  Audio: owns the piezo and hides LEDC, duty-cycle volume, and the fact that
//  there is only one voice. Games and the shell hand it an Sfx and forget.
//
//  Fits the existing service pattern -- System owns it beside Display, Storage
//  and Net; update() is polled once per tick and never blocks. Once a note is
//  running it is generated in hardware, so a busy game loop cannot make it
//  stutter and servicing costs nothing but a timestamp compare.
// ============================================================================

enum Volume : uint8_t { VOL_OFF = 0, VOL_LOW = 1, VOL_MED = 2, VOL_HIGH = 3 };

class Audio {
public:
  // Make AUDIO_PIN a plain output at AUDIO_IDLE_LEVEL -- the level at which the
  // FET is off -- and enable the pad's internal pull in the same direction.
  // Static and separate from begin() because it has to happen EARLIER than
  // begin() can: see audio.cpp. Call it first thing at boot, before any other
  // peripheral comes up.
  static void parkPin();

  void begin(Storage* store);        // loads the saved volume; parks the pin
  void update(uint32_t now);         // advance the current sound; call once per tick

  // Start `s`, subject to arbitration (below). Safe to call every tick.
  void play(const Sfx& s);
  void stop();                       // hard silence, pin at the idle level

  void   setVolume(Volume v, bool persist = true);
  Volume volume() const { return _vol; }
  bool   busy()   const { return _playing; }

private:
  void startStep(uint32_t now);
  void silence();

  Storage* _store  = nullptr;
  Volume   _vol    = VOL_MED;

  // The sound in flight. Copied BY VALUE so a caller may pass a temporary Sfx
  // wrapper; the Step array it points at must still be static (see sound.h).
  Sfx      _cur     = { nullptr, 0, 0 };
  bool     _playing = false;
  uint8_t  _idx     = 0;             // which step
  uint32_t _stepEndsAt = 0;

  // Whether LEDC currently owns the pin. It is claimed per note and handed back
  // in silence(), so the resting state is plain GPIO at a known level.
  bool     _attached = false;
};
