#include "audio.h"
#include "core/storage.h"
#include <driver/gpio.h>     // gpio_pullup_en / gpio_pulldown_en

#define AUDIO_VOL_KEY  "volume"

// Hold the pad's internal pull in the SAFE direction -- toward the level that
// switches the FET off, so up for an active-low gate. Kept enabled permanently,
// including while LEDC is driving: the pull is ~45k and the driver is push-pull,
// so it costs about 70 uA while a note sounds and changes nothing audible.
//
// What it buys is every instant the pin is NOT being driven. The gap between
// ledcDetach and the pinMode that follows it is the concrete one, but anything
// that reconfigures the pad as an input is covered too, and the failure
// direction of a bare active-low gate is ON.
//
// What it does NOT buy is the boot window. Pad pulls come out of reset disabled,
// so this only takes effect once parkPin() has run -- at which point the pin is
// being actively driven anyway. An external pull-up on the gate is still the
// only thing that covers reset, the ROM bootloader and flashing. It belongs on
// the buzzer board.
//
// Re-asserted after every ledcAttach rather than assumed: pinMode() sets the
// pull as part of gpio_config, but LEDC reconfigures the pad's direction and
// output routing on its way in and nothing promises it leaves the pull alone.
static inline void holdPull() {
#if AUDIO_ACTIVE_LOW
  gpio_pullup_en((gpio_num_t)AUDIO_PIN);
  gpio_pulldown_dis((gpio_num_t)AUDIO_PIN);
#else
  gpio_pulldown_en((gpio_num_t)AUDIO_PIN);
  gpio_pullup_dis((gpio_num_t)AUDIO_PIN);
#endif
}

// Indexed by Volume, in FET-ON time out of AUDIO_DUTY_FULL. VOL_OFF is 0.
static const uint16_t VOL_ON[4] = {
  0, AUDIO_DUTY_LOW, AUDIO_DUTY_MED, AUDIO_DUTY_HIGH
};

// LEDC duty counts time HIGH, but the gate conducts while the pin is LOW, so
// every duty leaves here complemented. The table above stays in the units a
// human reasons about -- how much of the cycle the FET is on -- and the one
// place that has to know about polarity is this function.
//
// The complement preserves loudness exactly: 50% inverted is still 50%, and
// 2% on-time becomes a 98% duty that is low for 2% of each period.
static inline uint32_t dutyOut(Volume v) {
#if AUDIO_ACTIVE_LOW
  return (uint32_t)AUDIO_DUTY_FULL - (uint32_t)VOL_ON[v];
#else
  return (uint32_t)VOL_ON[v];
#endif
}

// Hold the pin at the idle level using plain GPIO, before LEDC is involved.
//
// Split out of begin() because of WHEN it has to run. begin() needs Storage, to
// load the saved volume, and Storage comes up after the display and after the
// radio -- so if parking the pin waited for begin(), AUDIO_PIN would sit in its
// power-on reset state (a high-impedance input: GPIO 8 is a plain GPIO on the
// S3, not a strapping pin, so nothing pulls it either way) through all of
// FastLED init, NVS init and ESP-NOW bring-up.
//
// That is hundreds of milliseconds on every boot with nothing defining the gate.
// It matters more for an active-low part than a high-side N-channel one would:
// a floating gate that drifts DOWN turns a P-channel FET ON, so the failure
// direction of "unclaimed pin" is the expensive one. Calling this first closes
// the window to microseconds.
//
// It cannot close the window entirely: nothing in firmware runs during the ROM
// bootloader, and the internal pull is disabled out of reset too, so neither
// mechanism here covers that stretch. Only an external gate pull-up does.
//
// Idempotent, and every silent path ends in it. pinMode() first, because its
// gpio_config clears the pad's pulls -- so holdPull() has to follow it, not
// precede it.
void Audio::parkPin() {
  pinMode(AUDIO_PIN, OUTPUT);
  holdPull();
  digitalWrite(AUDIO_PIN, AUDIO_IDLE_LEVEL);
}

void Audio::begin(Storage* store) {
  parkPin();
  _store = store;

  uint32_t v = store ? store->getU32(STORAGE_NS_SYSTEM, AUDIO_VOL_KEY, VOL_MED) : VOL_MED;
  _vol = (v <= VOL_HIGH) ? (Volume)v : VOL_MED;

  // No ledcAttach here on purpose. LEDC is claimed per note and handed back in
  // silence(), so the resting state of this pin is plain GPIO at a known level
  // rather than a PWM channel we are trusting to render 100% duty exactly.
}

// Idle is a GPIO level, not a duty write.
//
// The obvious implementation is "write the duty that means fully off", and for
// an active-low gate that is FULL scale. But whether LEDC renders a full-scale
// request as a genuine constant level or as all-but-one-tick is a property of
// the driver, and all-but-one-tick would put a narrow ON pulse through the FET
// once per period, forever, while idle. Since idle is where this pin spends
// virtually its whole life, it gets the implementation that cannot be misread:
// take the pin back as GPIO and drive it.
void Audio::silence() {
  if (_attached) {
    ledcDetach(AUDIO_PIN);
    _attached = false;
  }
  parkPin();      // immediately: detaching alone would leave the gate floating
}

void Audio::stop() {
  _playing = false;
  silence();
}

void Audio::setVolume(Volume v, bool persist) {
  if (v > VOL_HIGH) v = VOL_HIGH;
  _vol = v;

  // Apply immediately if a note is sounding, so a volume preview is audible on
  // the note already in the air rather than only on the next one.
  bool sounding = _playing && _cur.steps[_idx].freq != 0 && _vol != VOL_OFF;
  if (sounding && _attached) ledcWrite(AUDIO_PIN, dutyOut(_vol));
  else if (!sounding)        silence();

  if (persist && _store) _store->putU32(STORAGE_NS_SYSTEM, AUDIO_VOL_KEY, (uint32_t)_vol);
}

// ---------------------------------------------------------------------------
//  Arbitration: one buzzer, one voice.
//
//  A higher-or-equal priority sound takes it; a lower one is DROPPED, not
//  queued. Queueing would be worse: stingers describe a moment, and a death
//  blip that arrives two seconds late -- during the victory screen -- is more
//  confusing than no blip at all. Stale is worse than absent.
//
//  The comparison is >= rather than > so a repeated sound retriggers. That is
//  what makes held-button menu scrolling feel responsive instead of muted after
//  the first click.
//
//  This is only half the flood defence, and the weaker half: it stops a LOW
//  priority sound interrupting a big one, but says nothing about a game firing
//  the same event twelve times in one tick. Collapsing that belongs to the
//  caller, at the point where it still knows the events are the same kind.
//  Virus does it in VirusApp (see virus_app.cpp).
// ---------------------------------------------------------------------------
void Audio::play(const Sfx& s) {
  if (s.steps == nullptr || s.count == 0) return;
  if (_playing && s.priority < _cur.priority) return;   // dropped, not deferred

  _cur     = s;
  _idx     = 0;
  _playing = true;
  startStep(millis());
}

void Audio::startStep(uint32_t now) {
  const Step& st = _cur.steps[_idx];

  if (st.freq == 0 || _vol == VOL_OFF) {
    silence();                       // a rest, or muted
  } else {
    // Attach at the note's own frequency, so there is no separate
    // change-frequency call and no window sounding at the previous pitch.
    //
    // LEDC starts a freshly attached channel at duty 0, which for an active-low
    // gate is FET-on, so there is a gap of a few microseconds here before the
    // duty write lands. Once per note, and microseconds of conduction is
    // nothing the element or the FET will notice -- unlike the milliseconds-to-
    // forever version this replaced.
    if (!_attached) {
      _attached = ledcAttach(AUDIO_PIN, st.freq, AUDIO_LEDC_BITS);
      if (_attached) holdPull();     // LEDC just reconfigured the pad
    } else {
      ledcChangeFrequency(AUDIO_PIN, st.freq, AUDIO_LEDC_BITS);
    }

    if (_attached) ledcWrite(AUDIO_PIN, dutyOut(_vol));
    else           parkPin();        // attach failed: stay off rather than guess
  }
  _stepEndsAt = now + st.ms;
}

void Audio::update(uint32_t now) {
  if (!_playing) return;

  // Signed difference so the comparison survives millis() wrapping at ~49 days.
  if ((int32_t)(now - _stepEndsAt) < 0) return;

  if (++_idx >= _cur.count) { stop(); return; }
  startStep(now);
}
